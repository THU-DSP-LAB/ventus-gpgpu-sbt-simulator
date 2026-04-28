#include "sbt/cfg.hpp"
#include "sbt/cfg_verify.hpp"
#include "sbt/control_semantics.hpp"
#include "sbt/elf_reader.hpp"
#include "sbt/ptx_emit.hpp"
#include "sbt/riscv_decode.hpp"
#include "spike_encoding_subset.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

static bool env_truthy(const char *k) {
  const char *v = std::getenv(k);
  if (!v)
    return false;
  std::string s(v);
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (s == "1" || s == "true" || s == "yes" || s == "on");
}

static bool env_bool(const char *k, bool default_value) {
  const char *v = std::getenv(k);
  if (!v)
    return default_value;
  std::string s(v);
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s == "1" || s == "true" || s == "yes" || s == "on")
    return true;
  if (s == "0" || s == "false" || s == "no" || s == "off")
    return false;
  return default_value;
}

static std::optional<std::string> env_str(const char *k) {
  const char *v = std::getenv(k);
  if (!v)
    return std::nullopt;
  std::string s(v);
  if (s.empty())
    return std::nullopt;
  return s;
}

static std::string json_escape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
        out += buf;
      } else {
        out += c;
      }
    }
  }
  return out;
}

static bool append_line_atomic(const fs::path &p, const std::string &line) {
  const std::string path = p.string();
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    return false;
  const std::string payload = line + "\n";
  const ssize_t n = ::write(fd, payload.data(), payload.size());
  ::close(fd);
  return n == static_cast<ssize_t>(payload.size());
}

static long long file_time_token(const fs::path &p) {
  // Note: file_time_type epoch is platform-specific, but we only use it for
  // equality checks.
  const auto ft = fs::last_write_time(p);
  const auto ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(ft)
                      .time_since_epoch()
                      .count();
  return static_cast<long long>(ns);
}

static std::optional<fs::path> self_exe_path() {
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (ec)
    return std::nullopt;
  return p;
}

static void usage() {
  std::cerr << "用法:\n";
  std::cerr << "  sbt_ptx <elf> --func <kernel> [--out <ptx>] [--sm <cc>]\n";
  std::cerr
      << "          [--require-known] [--no-bundle-regext] [--no-comments]\n";
  std::cerr << "          [--no-cache]\n";
}

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

static std::vector<sbt::Pattern> build_patterns_from_subset_header() {
  std::vector<sbt::Pattern> out;
  out.reserve(sizeof(sbt::gen::kPatterns) / sizeof(sbt::gen::kPatterns[0]));
  for (const auto &p : sbt::gen::kPatterns) {
    out.push_back({p.name, p.match, p.mask});
  }
  return out;
}

struct FuncRange final {
  uint32_t start = 0;
  uint32_t end = 0; // exclusive
};

static std::optional<FuncRange>
find_func_range(const std::vector<sbt::elf::FuncSymbol> &syms,
                uint32_t text_vaddr, uint32_t text_end,
                const std::string &name) {
  for (size_t i = 0; i < syms.size(); ++i) {
    if (syms[i].name != name)
      continue;
    const auto &s = syms[i];
    if (s.addr < text_vaddr || s.addr >= text_end)
      return std::nullopt;
    uint32_t start = s.addr;
    uint32_t end = 0;
    if (s.size != 0) {
      end = start + s.size;
    } else {
      for (size_t j = i + 1; j < syms.size(); ++j) {
        if (syms[j].addr > start) {
          end = syms[j].addr;
          break;
        }
      }
      if (end == 0)
        end = text_end;
    }
    if (end <= start)
      end = text_end;
    if (end > text_end)
      end = text_end;
    return FuncRange{start, end};
  }
  return std::nullopt;
}

static std::optional<FuncRange>
find_func_range_by_addr(const std::vector<sbt::elf::FuncSymbol> &syms,
                        uint32_t text_vaddr, uint32_t text_end, uint32_t addr) {
  auto it = std::lower_bound(
      syms.begin(), syms.end(), addr,
      [](const sbt::elf::FuncSymbol &a, uint32_t v) { return a.addr < v; });
  if (it == syms.end() || it->addr != addr)
    return std::nullopt;
  const size_t i = static_cast<size_t>(it - syms.begin());
  const auto &s = syms[i];
  if (s.addr < text_vaddr || s.addr >= text_end)
    return std::nullopt;
  const uint32_t start = s.addr;
  uint32_t end = 0;
  if (s.size != 0) {
    end = start + s.size;
  } else {
    for (size_t j = i + 1; j < syms.size(); ++j) {
      if (syms[j].addr > start) {
        end = syms[j].addr;
        break;
      }
    }
    if (end == 0)
      end = text_end;
  }
  if (end <= start)
    end = text_end;
  if (end > text_end)
    end = text_end;
  return FuncRange{start, end};
}

static std::string hex8(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", x);
  return std::string(buf);
}

static std::string sanitize_ptx_ident(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    const unsigned char uc = static_cast<unsigned char>(c);
    const bool ok = (std::isalnum(uc) != 0) || c == '_' || c == '$';
    if (ok) {
      out.push_back(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "_%02x", static_cast<unsigned>(uc));
      out += buf;
    }
  }
  if (out.empty())
    out = "anon";
  return out;
}

static std::string ptx_func_name_for(const std::string &ventus_sym,
                                     uint32_t start_addr) {
  return "__sbt_fn_" + sanitize_ptx_ident(ventus_sym) + "_" + hex8(start_addr);
}

static fs::path default_out_path(const fs::path &elf, const std::string &func) {
  const std::string bench = elf.parent_path().filename().string();
  const std::string stem = elf.stem().string();
  fs::path dir = "build/ptx";
  fs::path p = dir / (bench + "." + stem + "." + func + ".ptx");
  return p;
}

struct CacheKey final {
  std::string elf_abs;
  long long elf_time = 0;
  std::string func;
  fs::path out_abs;
  int sm = 0;
  bool require_known = false;
  bool bundle_regext = true;
  bool compat_spike_nested_regext = false;
  bool include_comments = true;
  std::string exe_abs;
  long long exe_time = 0;
};

static fs::path cache_meta_path_for(const fs::path &out_path) {
  return out_path.string() + ".meta";
}

static std::string render_cache_meta(const CacheKey &k) {
  std::ostringstream o;
  o << "format=2\n";
  o << "elf_abs=" << k.elf_abs << "\n";
  o << "elf_time=" << k.elf_time << "\n";
  o << "func=" << k.func << "\n";
  o << "out_abs=" << k.out_abs.string() << "\n";
  o << "sm=" << k.sm << "\n";
  o << "require_known=" << (k.require_known ? 1 : 0) << "\n";
  o << "bundle_regext=" << (k.bundle_regext ? 1 : 0) << "\n";
  o << "compat_spike_nested_regext=" << (k.compat_spike_nested_regext ? 1 : 0)
    << "\n";
  o << "include_comments=" << (k.include_comments ? 1 : 0) << "\n";
  o << "exe_abs=" << k.exe_abs << "\n";
  o << "exe_time=" << k.exe_time << "\n";
  return o.str();
}

static std::optional<std::string> read_text_file(const fs::path &p) {
  std::ifstream f(p);
  if (!f)
    return std::nullopt;
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

static std::optional<std::string> get_kv(const std::string &meta,
                                         const std::string &key) {
  const std::string pat = key + "=";
  size_t pos = 0;
  while (true) {
    const size_t line_start = meta.find(pat, pos);
    if (line_start == std::string::npos)
      return std::nullopt;
    if (line_start == 0 || meta[line_start - 1] == '\n') {
      const size_t val_start = line_start + pat.size();
      const size_t line_end = meta.find('\n', val_start);
      if (line_end == std::string::npos)
        return meta.substr(val_start);
      return meta.substr(val_start, line_end - val_start);
    }
    pos = line_start + 1;
  }
}

static bool cache_meta_matches(const CacheKey &k, const std::string &meta) {
  auto eqs = [&](const char *kk, const std::string &want) -> bool {
    auto got = get_kv(meta, kk);
    return got.has_value() && *got == want;
  };
  auto eql = [&](const char *kk, long long want) -> bool {
    auto got = get_kv(meta, kk);
    return got.has_value() && std::stoll(*got) == want;
  };
  auto eqi = [&](const char *kk, int want) -> bool {
    auto got = get_kv(meta, kk);
    return got.has_value() && std::stoi(*got) == want;
  };

  auto fmt = get_kv(meta, "format");
  if (!fmt || *fmt != "2")
    return false;

  return eqs("elf_abs", k.elf_abs) && eql("elf_time", k.elf_time) &&
         eqs("func", k.func) && eqs("out_abs", k.out_abs.string()) &&
         eqi("sm", k.sm) && eqi("require_known", k.require_known ? 1 : 0) &&
         eqi("bundle_regext", k.bundle_regext ? 1 : 0) &&
         eqi("compat_spike_nested_regext",
             k.compat_spike_nested_regext ? 1 : 0) &&
         eqi("include_comments", k.include_comments ? 1 : 0) &&
         eqs("exe_abs", k.exe_abs) && eql("exe_time", k.exe_time);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }

  fs::path elf_path;
  std::optional<std::string> func;
  fs::path out_path;
  bool require_known = false;
  bool bundle_regext = true;
  bool include_comments = true;
  int sm = 89;
  bool cache_enabled = true;
  const bool compat_spike_nested_regext =
      env_bool("SBT_COMPAT_SPIKE_NESTED_REGEXT", false);

  elf_path = argv[1];

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--func" && i + 1 < argc) {
      func = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--sm" && i + 1 < argc) {
      sm = std::stoi(argv[++i]);
    } else if (a == "--require-known") {
      require_known = true;
    } else if (a == "--no-bundle-regext") {
      bundle_regext = false;
    } else if (a == "--no-comments") {
      include_comments = false;
    } else if (a == "--no-cache") {
      cache_enabled = false;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "未知参数: " << a << "\n";
      usage();
      return 2;
    }
  }

  if (!func) {
    std::cerr << "缺少 --func <kernel>\n";
    usage();
    return 2;
  }
  if (out_path.empty())
    out_path = default_out_path(elf_path, *func);

  try {
    const auto profile_log = env_str("GPU_SBT_PTX_PROFILE_LOG");
    const bool profile = profile_log.has_value();
    const auto t0_total = std::chrono::steady_clock::now();

    if (env_truthy("GPU_SBT_PTX_NO_CACHE"))
      cache_enabled = false;

    CacheKey key;
    key.elf_abs = fs::absolute(elf_path).string();
    key.elf_time = file_time_token(elf_path);
    key.func = *func;
    key.out_abs = fs::absolute(out_path);
    key.sm = sm;
    key.require_known = require_known;
    key.bundle_regext = bundle_regext;
    key.compat_spike_nested_regext = compat_spike_nested_regext;
    key.include_comments = include_comments;
    if (auto exe = self_exe_path()) {
      key.exe_abs = fs::absolute(*exe).string();
      key.exe_time = file_time_token(*exe);
    } else {
      key.exe_abs = fs::absolute(fs::path(argv[0])).string();
      key.exe_time = file_time_token(fs::path(argv[0]));
    }

    bool cache_hit = false;
    double t_total_ms = 0.0;
    double t_work_ms = 0.0;
    double t_write_ms = 0.0;

    if (cache_enabled) {
      const fs::path out_abs = key.out_abs;
      const fs::path meta_path = cache_meta_path_for(out_abs);
      std::error_code ec;
      const bool out_ok = fs::exists(out_abs, ec) &&
                          fs::is_regular_file(out_abs, ec) &&
                          (fs::file_size(out_abs, ec) > 0);
      const bool meta_ok =
          fs::exists(meta_path, ec) && fs::is_regular_file(meta_path, ec);
      if (out_ok && meta_ok) {
        if (auto meta = read_text_file(meta_path)) {
          if (cache_meta_matches(key, *meta)) {
            cache_hit = true;
            const auto t1_total = std::chrono::steady_clock::now();
            t_total_ms = std::chrono::duration_cast<
                             std::chrono::duration<double, std::milli>>(
                             t1_total - t0_total)
                             .count();
            if (profile) {
              std::ostringstream j;
              j << "{\"ts_ms\":"
                << (long long)
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
              j << ",\"pid\":" << (long long)::getpid();
              j << ",\"elf\":\"" << json_escape(key.elf_abs) << "\"";
              j << ",\"func\":\"" << json_escape(key.func) << "\"";
              j << ",\"out\":\"" << json_escape(key.out_abs.string()) << "\"";
              j << ",\"sm\":" << key.sm;
              j << ",\"cache_hit\":true";
              j << ",\"t_total_ms\":" << t_total_ms;
              j << "}";
              append_line_atomic(*profile_log, j.str());
            }
            // Keep stdout compatible with existing driver usage.
            std::cout << "已生成 PTX: " << out_path << "\n";
            std::cout << "提示: 需要 dynamic shared >= warps_per_block*ldsStackSizePerWf + ldsSize（其中 ldsStackSizePerWf 来自 KNL metadata）\n";
            return 0;
          }
        }
      }
    }

    const auto t0_work = std::chrono::steady_clock::now();
    const auto text = sbt::elf::read_section(elf_path, ".text");
    const auto syms = sbt::elf::read_func_symbols(elf_path);

    std::unordered_map<uint32_t, std::string> sym_by_addr;
    sym_by_addr.reserve(syms.size());
    for (const auto &s : syms) {
      if (!s.name.empty())
        sym_by_addr.emplace(s.addr, s.name);
    }

    const uint32_t text_end =
        text.vaddr + static_cast<uint32_t>(text.data.size());
    const auto fr = find_func_range(syms, text.vaddr, text_end, *func);
    if (!fr) {
      std::cerr << "未找到函数符号或不在 .text: " << *func << "\n";
      return 1;
    }

    if (fr->start < text.vaddr || fr->end > text_end || fr->end <= fr->start) {
      std::cerr << "函数范围非法: " << *func << " start=" << hex_u32(fr->start)
                << " end=" << hex_u32(fr->end) << "\n";
      return 1;
    }

    const size_t off = fr->start - text.vaddr;
    const size_t len = fr->end - fr->start;
    std::vector<uint8_t> slice(text.data.begin() + static_cast<long>(off),
                               text.data.begin() +
                                   static_cast<long>(off + len));

    const auto patterns = build_patterns_from_subset_header();

    sbt::DecodeOptions dopt;
    dopt.bundle_regext = bundle_regext;
    dopt.require_known = require_known;
    dopt.spike_compat_nested_regext = compat_spike_nested_regext;
    const auto decoded = sbt::decode_text(slice, fr->start, dopt, patterns);

    const auto cfg = sbt::cfg::build_function_cfg(decoded, fr->start, fr->end);
    const sbt::cfg::VerifyOptions verify_options{.sym_by_addr = &sym_by_addr};
    const auto verify = sbt::cfg::verify_function(cfg, *func, verify_options);

    const bool vbranch_ok = std::all_of(
        verify.vbranch.begin(), verify.vbranch.end(),
        [](const sbt::cfg::VBranchCheck &c) { return c.error.empty(); });
    const bool barrier_ok =
        std::all_of(verify.barriers.begin(), verify.barriers.end(),
                    [](const sbt::cfg::BarrierCheck &c) { return c.ok; });
    const bool jalr_ok = verify.unsupported_jalr.empty();
    if (!vbranch_ok || !barrier_ok || !jalr_ok) {
      std::cerr << "CFG 结构化验证未通过: " << *func << "\n";
      std::cerr << "  vbranch_ok=" << (vbranch_ok ? "true" : "false")
                << " barrier_ok=" << (barrier_ok ? "true" : "false")
                << " jalr_ok=" << (jalr_ok ? "true" : "false") << "\n";
      return 2;
    }

    sbt::ptx::Options popt;
    popt.sm = sm;
    popt.include_comments = include_comments;
    if (const auto gp = sbt::elf::read_symbol_value(elf_path, "__global_pointer$")) {
      popt.global_pointer_vaddr = *gp;
    }

    auto build_cfg_for = [&](const FuncRange &fr,
                             const std::string &name) -> sbt::cfg::FunctionCfg {
      if (fr.start < text.vaddr || fr.end > text_end || fr.end <= fr.start) {
        throw std::runtime_error("函数范围非法: " + name + " start=" +
                                 hex_u32(fr.start) + " end=" + hex_u32(fr.end));
      }
      const size_t off = fr.start - text.vaddr;
      const size_t len = fr.end - fr.start;
      std::vector<uint8_t> slice(text.data.begin() + static_cast<long>(off),
                                 text.data.begin() +
                                     static_cast<long>(off + len));
      const auto decoded = sbt::decode_text(slice, fr.start, dopt, patterns);
      const auto cfg = sbt::cfg::build_function_cfg(decoded, fr.start, fr.end);
      const auto verify = sbt::cfg::verify_function(cfg, name, verify_options);

      const bool vbranch_ok = std::all_of(
          verify.vbranch.begin(), verify.vbranch.end(),
          [](const sbt::cfg::VBranchCheck &c) { return c.error.empty(); });
      const bool barrier_ok =
          std::all_of(verify.barriers.begin(), verify.barriers.end(),
                      [](const sbt::cfg::BarrierCheck &c) { return c.ok; });
      const bool jalr_ok = verify.unsupported_jalr.empty();
      if (!vbranch_ok || !barrier_ok || !jalr_ok) {
        std::ostringstream o;
        o << "CFG 结构化验证未通过: " << name
          << " vbranch_ok=" << (vbranch_ok ? "true" : "false")
          << " barrier_ok=" << (barrier_ok ? "true" : "false")
          << " jalr_ok=" << (jalr_ok ? "true" : "false");
        throw std::runtime_error(o.str());
      }
      return cfg;
    };

    // Build a reachable set of direct-call callees and emit one PTX module:
    //   1 `.entry <kernel>` + N `.func` (direct callees).
    struct Node final {
      std::string name;
      FuncRange range{};
      sbt::cfg::FunctionCfg cfg{};
      std::vector<uint32_t> callees;
    };

    std::unordered_map<uint32_t, Node> nodes_by_start;
    nodes_by_start.reserve(64);
    std::vector<uint32_t> work;

    Node entry;
    entry.name = *func;
    entry.range = *fr;
    entry.cfg = cfg;
    nodes_by_start.emplace(fr->start, std::move(entry));
    work.push_back(fr->start);

    auto scan_callees = [&](const Node &n) -> std::vector<uint32_t> {
      std::vector<uint32_t> out;
      for (uint32_t target :
           sbt::control::collect_direct_call_targets(n.cfg, n.name)) {
        auto it = sym_by_addr.find(target);
        if (it == sym_by_addr.end()) {
          throw std::runtime_error("call target not in .symtab in " + n.name +
                                   " target=" + hex_u32(target));
        }
        const std::string &callee = it->second;
        if (sbt::ptx::is_inlined_builtin_call_name(callee))
          continue;
        out.push_back(target);
      }
      return out;
    };

    while (!work.empty()) {
      const uint32_t cur = work.back();
      work.pop_back();

      auto it = nodes_by_start.find(cur);
      if (it == nodes_by_start.end())
        continue;
      Node &n = it->second;
      n.callees = scan_callees(n);

      for (uint32_t callee_start : n.callees) {
        if (nodes_by_start.find(callee_start) != nodes_by_start.end())
          continue;

        auto fr2 =
            find_func_range_by_addr(syms, text.vaddr, text_end, callee_start);
        if (!fr2) {
          auto itn = sym_by_addr.find(callee_start);
          const std::string name2 =
              (itn != sym_by_addr.end() ? itn->second
                                        : std::string("(unknown)"));
          throw std::runtime_error("callee missing symbol range: " + name2 +
                                   " start=" + hex_u32(callee_start));
        }
        auto itn = sym_by_addr.find(callee_start);
        if (itn == sym_by_addr.end()) {
          throw std::runtime_error("callee missing symbol name start=" +
                                   hex_u32(callee_start));
        }
        const std::string &name2 = itn->second;

        Node nn;
        nn.name = name2;
        nn.range = *fr2;
        nn.cfg = build_cfg_for(*fr2, name2);
        nodes_by_start.emplace(callee_start, std::move(nn));
        work.push_back(callee_start);
      }
    }

    // Build adjacency and reject cycles (prototype: no recursion / mutual
    // recursion).
    enum class Mark : uint8_t { White = 0, Gray = 1, Black = 2 };
    std::unordered_map<uint32_t, Mark> mark;
    mark.reserve(nodes_by_start.size());
    for (const auto &kv : nodes_by_start)
      mark.emplace(kv.first, Mark::White);

    std::vector<uint32_t> stack;
    stack.reserve(nodes_by_start.size());
    std::function<bool(uint32_t)> dfs = [&](uint32_t u) -> bool {
      mark[u] = Mark::Gray;
      stack.push_back(u);
      const auto &n = nodes_by_start.at(u);
      for (uint32_t v : n.callees) {
        if (nodes_by_start.find(v) == nodes_by_start.end())
          continue;
        if (mark[v] == Mark::Gray)
          return true;
        if (mark[v] == Mark::White) {
          if (dfs(v))
            return true;
        }
      }
      stack.pop_back();
      mark[u] = Mark::Black;
      return false;
    };
    if (dfs(fr->start)) {
      throw std::runtime_error(
          "recursive call graph detected (unsupported in prototype)");
    }

    // Prepare `.func` emission list and call-target mapping.
    std::vector<uint32_t> starts;
    starts.reserve(nodes_by_start.size());
    for (const auto &kv : nodes_by_start) {
      if (kv.first == fr->start)
        continue;
      starts.push_back(kv.first);
    }
    std::sort(starts.begin(), starts.end());

    std::vector<sbt::ptx::FuncToEmit> funcs_to_emit;
    funcs_to_emit.reserve(starts.size());
    std::unordered_map<uint32_t, std::string> ptx_name_by_addr;
    ptx_name_by_addr.reserve(starts.size());

    for (uint32_t saddr : starts) {
      const auto &n = nodes_by_start.at(saddr);
      const std::string ptx_name = ptx_func_name_for(n.name, saddr);
      ptx_name_by_addr.emplace(saddr, ptx_name);

      sbt::ptx::FuncToEmit f;
      f.name = n.name;
      f.ptx_name = ptx_name;
      f.cfg = n.cfg;
      funcs_to_emit.push_back(std::move(f));
    }

    const auto res = sbt::ptx::emit_module(
        cfg, sym_by_addr, *func, funcs_to_emit, ptx_name_by_addr, popt);

    const auto t1_work = std::chrono::steady_clock::now();
    t_work_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            t1_work - t0_work)
            .count();

    const auto t0_write = std::chrono::steady_clock::now();
    if (!out_path.parent_path().empty()) {
      fs::create_directories(out_path.parent_path());
    }
    std::ofstream f(out_path);
    if (!f) {
      std::cerr << "无法写入: " << out_path << "\n";
      return 1;
    }
    f << res.ptx;
    f.close();
    const auto t1_write = std::chrono::steady_clock::now();
    t_write_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            t1_write - t0_write)
            .count();

    if (cache_enabled) {
      const fs::path out_abs = key.out_abs;
      const fs::path meta_path = cache_meta_path_for(out_abs);
      const fs::path tmp =
          meta_path.string() + ".tmp." + std::to_string(::getpid());
      const std::string body = render_cache_meta(key);
      {
        std::ofstream mf(tmp);
        if (mf) {
          mf << body;
          mf.close();
          std::error_code ec;
          fs::rename(tmp, meta_path, ec);
          if (ec) {
            // Best effort: if rename fails, try to clean tmp.
            fs::remove(tmp, ec);
          }
        }
      }
    }

    const auto t1_total = std::chrono::steady_clock::now();
    t_total_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            t1_total - t0_total)
            .count();

    if (profile) {
      std::ostringstream j;
      j << "{\"ts_ms\":"
        << (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count();
      j << ",\"pid\":" << (long long)::getpid();
      j << ",\"elf\":\"" << json_escape(key.elf_abs) << "\"";
      j << ",\"func\":\"" << json_escape(key.func) << "\"";
      j << ",\"out\":\"" << json_escape(key.out_abs.string()) << "\"";
      j << ",\"sm\":" << key.sm;
      j << ",\"cache_hit\":false";
      j << ",\"t_total_ms\":" << t_total_ms;
      j << ",\"t_work_ms\":" << t_work_ms;
      j << ",\"t_write_ms\":" << t_write_ms;
      j << "}";
      append_line_atomic(*profile_log, j.str());
    }

    std::cout << "已生成 PTX: " << out_path << "\n";
    std::cout << "提示: 需要 dynamic shared >= warps_per_block*ldsStackSizePerWf + ldsSize（其中 ldsStackSizePerWf 来自 KNL metadata）\n";
    return 0;
  } catch (const sbt::ptx::EmitError &e) {
    std::cerr << "PTX 生成失败: " << e.what() << "\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << "\n";
    return 1;
  }
}
