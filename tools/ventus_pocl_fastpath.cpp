#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace {

static bool fastpath_enabled() {
  static int enabled = -1;
  if (enabled != -1) return enabled != 0;
  const char *v = getenv("VENTUS_POCL_FASTPATH");
  enabled = (v && *v && std::string(v) != "0") ? 1 : 0;
  return enabled != 0;
}

static void *load_sym_next(const char *name) { return dlsym(RTLD_NEXT, name); }

static bool contains(const char *s, const char *needle) {
  if (!s || !needle) return false;
  return strstr(s, needle) != nullptr;
}

static std::optional<std::string> parse_nm_kernel(const std::string &cmd, std::string *out_file) {
  // Expected shape:
  //   nm -s object<N>.riscv | grep -w 'T' | grep -w <kernel> | grep -o '^[^ ]*'
  const char *p = strstr(cmd.c_str(), "nm -s ");
  if (!p) return std::nullopt;
  p += strlen("nm -s ");
  while (*p == ' ') ++p;
  const char *end = strpbrk(p, " \t|");
  if (!end) return std::nullopt;
  std::string file(p, (size_t)(end - p));
  if (file.size() < 6 || file.find(".riscv") == std::string::npos) return std::nullopt;

  size_t pos = cmd.rfind("grep -w ");
  if (pos == std::string::npos) return std::nullopt;
  pos += strlen("grep -w ");
  while (pos < cmd.size() && cmd[pos] == ' ') ++pos;
  if (pos >= cmd.size()) return std::nullopt;

  // Strip optional quotes.
  char quote = 0;
  if (cmd[pos] == '"' || cmd[pos] == '\'') {
    quote = cmd[pos];
    ++pos;
  }

  size_t k_end = pos;
  if (quote) {
    k_end = cmd.find(quote, pos);
    if (k_end == std::string::npos) return std::nullopt;
  } else {
    while (k_end < cmd.size()) {
      const char c = cmd[k_end];
      if (c == ' ' || c == '\t' || c == '|' || c == '\n' || c == '\r') break;
      ++k_end;
    }
  }
  std::string kernel = cmd.substr(pos, k_end - pos);
  if (kernel.empty() || kernel == "T") return std::nullopt;

  if (out_file) *out_file = file;
  return kernel;
}

// Minimal ELF32 parser for symbol address (avoids spawning nm/grep).
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct Elf32_Shdr {
  uint32_t sh_name;
  uint32_t sh_type;
  uint32_t sh_flags;
  uint32_t sh_addr;
  uint32_t sh_offset;
  uint32_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint32_t sh_addralign;
  uint32_t sh_entsize;
};

struct Elf32_Sym {
  uint32_t st_name;
  uint32_t st_value;
  uint32_t st_size;
  unsigned char st_info;
  unsigned char st_other;
  uint16_t st_shndx;
};

static std::optional<uint32_t> elf32_find_sym_addr(const std::string &path, const std::string &sym) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return std::nullopt;

  Elf32_Ehdr eh{};
  if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh)) {
    fclose(f);
    return std::nullopt;
  }
  if (!(eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' && eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F')) {
    fclose(f);
    return std::nullopt;
  }
  // EI_CLASS=1 (32-bit), EI_DATA=1 (LE)
  if (eh.e_ident[4] != 1 || eh.e_ident[5] != 1) {
    fclose(f);
    return std::nullopt;
  }
  if (eh.e_shoff == 0 || eh.e_shentsize == 0 || eh.e_shnum == 0) {
    fclose(f);
    return std::nullopt;
  }

  if (fseek(f, (long)eh.e_shoff, SEEK_SET) != 0) {
    fclose(f);
    return std::nullopt;
  }
  std::vector<Elf32_Shdr> shdrs(eh.e_shnum);
  for (size_t i = 0; i < shdrs.size(); ++i) {
    if (fread(&shdrs[i], 1, sizeof(Elf32_Shdr), f) != sizeof(Elf32_Shdr)) {
      fclose(f);
      return std::nullopt;
    }
  }

  // Find .symtab and its linked string table.
  const uint32_t SHT_SYMTAB = 2;
  int symtab_idx = -1;
  for (size_t i = 0; i < shdrs.size(); ++i) {
    if (shdrs[i].sh_type == SHT_SYMTAB) {
      symtab_idx = (int)i;
      break;
    }
  }
  if (symtab_idx < 0) {
    fclose(f);
    return std::nullopt;
  }

  const Elf32_Shdr &symtab = shdrs[(size_t)symtab_idx];
  if (symtab.sh_entsize == 0) {
    fclose(f);
    return std::nullopt;
  }
  const uint32_t nsyms = symtab.sh_size / symtab.sh_entsize;
  if (symtab.sh_link >= shdrs.size()) {
    fclose(f);
    return std::nullopt;
  }
  const Elf32_Shdr &strtab = shdrs[symtab.sh_link];

  std::vector<char> str(strtab.sh_size);
  if (fseek(f, (long)strtab.sh_offset, SEEK_SET) != 0) {
    fclose(f);
    return std::nullopt;
  }
  if (!str.empty() && fread(str.data(), 1, str.size(), f) != str.size()) {
    fclose(f);
    return std::nullopt;
  }

  if (fseek(f, (long)symtab.sh_offset, SEEK_SET) != 0) {
    fclose(f);
    return std::nullopt;
  }
  for (uint32_t i = 0; i < nsyms; ++i) {
    Elf32_Sym s{};
    if (fread(&s, 1, sizeof(s), f) != sizeof(s)) {
      fclose(f);
      return std::nullopt;
    }
    if (s.st_name >= str.size()) continue;
    const char *name = str.data() + s.st_name;
    if (!name) continue;
    if (sym == name) {
      fclose(f);
      return s.st_value;
    }
  }

  fclose(f);
  return std::nullopt;
}

static std::mutex g_mu;
static std::unordered_map<FILE *, char *> g_fake_pipes;

} // namespace

extern "C" int system(const char *command) {
  using Fn = int (*)(const char *);
  static Fn real = nullptr;
  if (!real) real = reinterpret_cast<Fn>(load_sym_next("system"));
  if (!real) {
    errno = ENOSYS;
    return -1;
  }

  if (fastpath_enabled() && command) {
    // PoCL Ventus path generates a .vmem via assemble.sh for non-PTX backends; PTX backend doesn't consume it.
    if (contains(command, "assemble.sh")) {
      return 0;
    }
  }
  return real(command);
}

extern "C" FILE *popen(const char *command, const char *type) {
  using Fn = FILE *(*)(const char *, const char *);
  static Fn real = nullptr;
  if (!real) real = reinterpret_cast<Fn>(load_sym_next("popen"));
  if (!real) return nullptr;

  if (fastpath_enabled() && command && type && type[0] == 'r') {
    std::string file;
    const std::string cmd(command);
    auto kernel = parse_nm_kernel(cmd, &file);
    if (kernel) {
      const auto addr = elf32_find_sym_addr(file, *kernel);
      char tmp[64];
      if (addr) {
        snprintf(tmp, sizeof(tmp), "0x%08x\n", (unsigned)*addr);
      } else {
        // Fallback: avoid leaving kernel_entry uninitialized in pocl_ventus.cc.
        snprintf(tmp, sizeof(tmp), "0x%08x\n", 0u);
      }
      const size_t n = strlen(tmp);
      char *buf = (char *)malloc(n + 1);
      if (!buf) return nullptr;
      memcpy(buf, tmp, n + 1);
      FILE *fp = fmemopen(buf, n, "r");
      if (fp) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_fake_pipes.emplace(fp, buf);
      } else {
        free(buf);
      }
      return fp;
    }
  }

  return real(command, type);
}

extern "C" int pclose(FILE *stream) {
  using Fn = int (*)(FILE *);
  static Fn real = nullptr;
  if (!real) real = reinterpret_cast<Fn>(load_sym_next("pclose"));
  if (!real) {
    errno = ENOSYS;
    return -1;
  }

  if (fastpath_enabled() && stream) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_fake_pipes.find(stream);
    if (it != g_fake_pipes.end()) {
      char *buf = it->second;
      g_fake_pipes.erase(it);
      fclose(stream);
      free(buf);
      return 0;
    }
  }
  return real(stream);
}
