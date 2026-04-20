#include "sbt/elf_reader.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <unistd.h>

#include <algorithm>

namespace sbt::elf {
namespace {

class Fd final {
public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) close(fd_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  int get() const { return fd_; }

private:
  int fd_ = -1;
};

class ElfHandle final {
public:
  explicit ElfHandle(Elf *e) : e_(e) {}
  ~ElfHandle() {
    if (e_) elf_end(e_);
  }
  ElfHandle(const ElfHandle &) = delete;
  ElfHandle &operator=(const ElfHandle &) = delete;
  Elf *get() const { return e_; }

private:
  Elf *e_ = nullptr;
};

static void require_elf_ok(bool ok, const char *msg) {
  if (!ok) {
    throw ElfError(std::string(msg) + ": " + elf_errmsg(-1));
  }
}

static void require(bool ok, const std::string &msg) {
  if (!ok) throw ElfError(msg);
}

static std::string errno_str() {
  const char *s = strerrordesc_np(errno);
  return (s ? std::string(s) : std::string("errno"));
}

static ElfHandle open_elf_ro(const std::filesystem::path &p, Fd &out_fd) {
  if (elf_version(EV_CURRENT) == EV_NONE) {
    throw ElfError("ELF: elf_version failed");
  }
  int fd = open(p.c_str(), O_RDONLY);
  if (fd < 0) {
    throw ElfError("ELF: cannot open '" + p.string() + "': " + errno_str());
  }
  out_fd = Fd(fd);
  Elf *e = elf_begin(fd, ELF_C_READ, nullptr);
  require_elf_ok(e != nullptr, "ELF: elf_begin failed");
  return ElfHandle(e);
}

static size_t get_shstrndx(Elf *e) {
  size_t shstrndx = 0;
  require_elf_ok(elf_getshdrstrndx(e, &shstrndx) == 0, "ELF: elf_getshdrstrndx failed");
  return shstrndx;
}

static std::string scn_name(Elf *e, size_t shstrndx, GElf_Shdr &shdr) {
  const char *name = elf_strptr(e, shstrndx, shdr.sh_name);
  if (name == nullptr) return "";
  return std::string(name);
}

static std::vector<uint8_t> collect_section_bytes(Elf_Scn *scn) {
  std::vector<uint8_t> out;
  for (Elf_Data *d = elf_getdata(scn, nullptr); d != nullptr; d = elf_getdata(scn, d)) {
    if (d->d_buf == nullptr || d->d_size == 0) continue;
    const auto *buf = static_cast<const uint8_t *>(d->d_buf);
    out.insert(out.end(), buf, buf + d->d_size);
  }
  return out;
}

} // namespace

ElfSection read_section(const std::filesystem::path &elf_path, std::string_view section_name) {
  Fd fd(-1);
  ElfHandle eh = open_elf_ro(elf_path, fd);
  Elf *e = eh.get();

  const size_t shstrndx = get_shstrndx(e);

  for (Elf_Scn *scn = elf_nextscn(e, nullptr); scn != nullptr; scn = elf_nextscn(e, scn)) {
    GElf_Shdr shdr{};
    require_elf_ok(gelf_getshdr(scn, &shdr) != nullptr, "ELF: gelf_getshdr failed");
    const std::string name = scn_name(e, shstrndx, shdr);
    if (name != section_name) continue;

    ElfSection out;
    out.name = name;
    out.vaddr = static_cast<uint32_t>(shdr.sh_addr);
    out.data = collect_section_bytes(scn);

    // Best-effort check.
    if (out.data.size() != shdr.sh_size) {
      // Keep going; some toolchains may split the data.
    }
    return out;
  }

  throw ElfError("ELF: section not found: '" + std::string(section_name) + "' in " + elf_path.string());
}

std::vector<FuncSymbol> read_func_symbols(const std::filesystem::path &elf_path) {
  Fd fd(-1);
  ElfHandle eh = open_elf_ro(elf_path, fd);
  Elf *e = eh.get();

  const size_t shstrndx = get_shstrndx(e);

  Elf_Scn *symtab_scn = nullptr;
  GElf_Shdr symtab_shdr{};

  for (Elf_Scn *scn = elf_nextscn(e, nullptr); scn != nullptr; scn = elf_nextscn(e, scn)) {
    GElf_Shdr shdr{};
    require_elf_ok(gelf_getshdr(scn, &shdr) != nullptr, "ELF: gelf_getshdr failed");
    const std::string name = scn_name(e, shstrndx, shdr);
    if (name == ".symtab") {
      symtab_scn = scn;
      symtab_shdr = shdr;
      break;
    }
  }

  require(symtab_scn != nullptr, "ELF: missing .symtab: " + elf_path.string());
  require(symtab_shdr.sh_entsize != 0, "ELF: .symtab sh_entsize is 0: " + elf_path.string());

  const size_t strtab_ndx = symtab_shdr.sh_link;

  Elf_Data *sym_data = elf_getdata(symtab_scn, nullptr);
  require_elf_ok(sym_data != nullptr, "ELF: elf_getdata(.symtab) failed");

  const size_t sym_count = symtab_shdr.sh_size / symtab_shdr.sh_entsize;
  std::vector<FuncSymbol> out;
  out.reserve(sym_count / 4);

  for (size_t i = 0; i < sym_count; ++i) {
    GElf_Sym sym{};
    require_elf_ok(gelf_getsym(sym_data, static_cast<int>(i), &sym) != nullptr, "ELF: gelf_getsym failed");
    const unsigned st_type = GELF_ST_TYPE(sym.st_info);
    const unsigned st_bind = GELF_ST_BIND(sym.st_info);
    const unsigned st_vis = GELF_ST_VISIBILITY(sym.st_other);

    if (st_type != STT_FUNC) continue;
    if (sym.st_shndx == SHN_UNDEF) continue;

    const char *name = elf_strptr(e, strtab_ndx, sym.st_name);
    if (name == nullptr || name[0] == '\0') continue;

    FuncSymbol fs;
    fs.name = name;
    fs.addr = static_cast<uint32_t>(sym.st_value);
    fs.size = static_cast<uint32_t>(sym.st_size);
    fs.bind = static_cast<uint8_t>(st_bind);
    fs.type = static_cast<uint8_t>(st_type);
    fs.vis = static_cast<uint8_t>(st_vis);
    fs.shndx = sym.st_shndx;
    out.push_back(std::move(fs));
  }

  std::sort(out.begin(), out.end(), [](const FuncSymbol &a, const FuncSymbol &b) {
    if (a.addr != b.addr) return a.addr < b.addr;
    return a.name < b.name;
  });
  return out;
}

std::optional<uint32_t> read_symbol_value(const std::filesystem::path &elf_path, std::string_view symbol_name) {
  Fd fd(-1);
  ElfHandle eh = open_elf_ro(elf_path, fd);
  Elf *e = eh.get();

  const size_t shstrndx = get_shstrndx(e);

  Elf_Scn *symtab_scn = nullptr;
  GElf_Shdr symtab_shdr{};
  for (Elf_Scn *scn = elf_nextscn(e, nullptr); scn != nullptr; scn = elf_nextscn(e, scn)) {
    GElf_Shdr shdr{};
    require_elf_ok(gelf_getshdr(scn, &shdr) != nullptr, "ELF: gelf_getshdr failed");
    if (scn_name(e, shstrndx, shdr) != ".symtab") continue;
    symtab_scn = scn;
    symtab_shdr = shdr;
    break;
  }

  require(symtab_scn != nullptr, "ELF: missing .symtab: " + elf_path.string());
  require(symtab_shdr.sh_entsize != 0, "ELF: .symtab sh_entsize is 0: " + elf_path.string());

  const size_t strtab_ndx = symtab_shdr.sh_link;
  Elf_Data *sym_data = elf_getdata(symtab_scn, nullptr);
  require_elf_ok(sym_data != nullptr, "ELF: elf_getdata(.symtab) failed");

  const size_t sym_count = symtab_shdr.sh_size / symtab_shdr.sh_entsize;
  for (size_t i = 0; i < sym_count; ++i) {
    GElf_Sym sym{};
    require_elf_ok(gelf_getsym(sym_data, static_cast<int>(i), &sym) != nullptr, "ELF: gelf_getsym failed");
    if (sym.st_shndx == SHN_UNDEF) continue;

    const char *name = elf_strptr(e, strtab_ndx, sym.st_name);
    if (name == nullptr || name[0] == '\0') continue;
    if (name != symbol_name) continue;
    if (sym.st_value > UINT32_MAX) {
      throw ElfError(
          "ELF: symbol value out of 32-bit range: '" + std::string(symbol_name) + "' in " + elf_path.string()
      );
    }
    return static_cast<uint32_t>(sym.st_value);
  }
  return std::nullopt;
}

} // namespace sbt::elf
