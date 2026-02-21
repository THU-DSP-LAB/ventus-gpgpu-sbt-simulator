#include "sbt/cfg.hpp"
#include "sbt/cfg_verify.hpp"
#include "sbt/elf_reader.hpp"
#include "sbt/ptx_emit.hpp"
#include "sbt/riscv_decode.hpp"
#include "sbt/spike_encoding_parser.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
  if (!v) return false;
  std::string s(v);
  for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (s == "1" || s == "true" || s == "yes" || s == "on");
}

static std::optional<std::string> env_str(const char *k) {
  const char *v = std::getenv(k);
  if (!v) return std::nullopt;
  std::string s(v);
  if (s.empty()) return std::nullopt;
  return s;
}

static std::string json_escape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
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
  if (fd < 0) return false;
  const std::string payload = line + "\n";
  const ssize_t n = ::write(fd, payload.data(), payload.size());
  ::close(fd);
  return n == static_cast<ssize_t>(payload.size());
}

static long long file_time_token(const fs::path &p) {
  // Note: file_time_type epoch is platform-specific, but we only use it for equality checks.
  const auto ft = fs::last_write_time(p);
  const auto ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(ft).time_since_epoch().count();
  return static_cast<long long>(ns);
}

static std::optional<fs::path> self_exe_path() {
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return std::nullopt;
  return p;
}

static void usage() {
  std::cerr << "用法:\n";
  std::cerr << "  sbt_ptx <elf> --func <kernel> [--out <ptx>] [--sm <cc>] [--encoding-h <path>]\n";
  std::cerr << "          [--require-known] [--no-bundle-regext] [--no-comments]\n";
  std::cerr << "          [--no-cache]\n";
}

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

struct PatternPack final {
  std::vector<std::string> names;
  std::vector<sbt::Pattern> patterns;
};

static PatternPack build_patterns_from_encoding(const fs::path &encoding_h_path) {
  const auto decl = sbt::spike::parse_declared_insns(encoding_h_path);

  const std::vector<std::string> want = {
      "regext",     "regexti",   "setrpc",     "join",      "endprg",    "barrier",
      "vbeq",       "vbne",      "vblt",       "vbge",      "vbltu",     "vbgeu",
      "vlw12_v",    "vsw12_v",   "vlbu12_v",   "vsb12_v",   "vlw_v",     "vsw_v",
      "vadd12_vi",  "vsub12_vi", "vid_v",      "vmv_v_x",   "vsetvli",   "vadd_vv",   "vadd_vx",
      "vadd_vi",    "vsub_vv",   "vand_vv",    "vand_vi",   "vor_vv",    "vxor_vi",   "vsll_vi",
      "vsrl_vi",    "vsra_vi",   "vmul_vx",    "vmulh_vx",  "vdivu_vx",  "vremu_vx",  "vmadd_vv",
      "vmadd_vx",   "vmflt_vv",  "vmslt_vx",   "vmsltu_vx", "vfadd_vv",  "vfsub_vv",
      "vfmul_vv",   "vfdiv_vv",  "vfmadd_vv",  "vfsqrt_v",  "vfsgnjn_vv",
  };

  PatternPack out;
  out.names.reserve(want.size());
  out.patterns.reserve(want.size());

  for (const auto &n : want) {
    auto it = decl.find(n);
    if (it == decl.end()) {
      throw std::runtime_error("encoding.h 缺少 DECLARE_INSN: " + n);
    }
    out.names.push_back(it->second.name);
    sbt::Pattern ptn;
    ptn.name = out.names.back().c_str();
    ptn.match = it->second.match;
    ptn.mask = it->second.mask;
    out.patterns.push_back(ptn);
  }

  return out;
}

struct FuncRange final {
  uint32_t start = 0;
  uint32_t end = 0; // exclusive
};

static std::optional<FuncRange> find_func_range(const std::vector<sbt::elf::FuncSymbol> &syms, uint32_t text_vaddr, uint32_t text_end,
                                                const std::string &name) {
  for (size_t i = 0; i < syms.size(); ++i) {
    if (syms[i].name != name) continue;
    const auto &s = syms[i];
    if (s.addr < text_vaddr || s.addr >= text_end) return std::nullopt;
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
      if (end == 0) end = text_end;
    }
    if (end <= start) end = text_end;
    if (end > text_end) end = text_end;
    return FuncRange{start, end};
  }
  return std::nullopt;
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
  bool include_comments = true;
  bool scalar_leader_only = false;
  std::string encoding_h_abs;
  long long encoding_time = 0;
  std::string exe_abs;
  long long exe_time = 0;
};

static fs::path cache_meta_path_for(const fs::path &out_path) {
  return out_path.string() + ".meta";
}

static std::string render_cache_meta(const CacheKey &k) {
  std::ostringstream o;
  o << "format=1\n";
  o << "elf_abs=" << k.elf_abs << "\n";
  o << "elf_time=" << k.elf_time << "\n";
  o << "func=" << k.func << "\n";
  o << "out_abs=" << k.out_abs.string() << "\n";
  o << "sm=" << k.sm << "\n";
  o << "require_known=" << (k.require_known ? 1 : 0) << "\n";
  o << "bundle_regext=" << (k.bundle_regext ? 1 : 0) << "\n";
  o << "include_comments=" << (k.include_comments ? 1 : 0) << "\n";
  o << "scalar_leader_only=" << (k.scalar_leader_only ? 1 : 0) << "\n";
  o << "encoding_h_abs=" << k.encoding_h_abs << "\n";
  o << "encoding_time=" << k.encoding_time << "\n";
  o << "exe_abs=" << k.exe_abs << "\n";
  o << "exe_time=" << k.exe_time << "\n";
  return o.str();
}

static std::optional<std::string> read_text_file(const fs::path &p) {
  std::ifstream f(p);
  if (!f) return std::nullopt;
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

static std::optional<std::string> get_kv(const std::string &meta, const std::string &key) {
  const std::string pat = key + "=";
  size_t pos = 0;
  while (true) {
    const size_t line_start = meta.find(pat, pos);
    if (line_start == std::string::npos) return std::nullopt;
    if (line_start == 0 || meta[line_start - 1] == '\n') {
      const size_t val_start = line_start + pat.size();
      const size_t line_end = meta.find('\n', val_start);
      if (line_end == std::string::npos) return meta.substr(val_start);
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
  if (!fmt || *fmt != "1") return false;

  return eqs("elf_abs", k.elf_abs) && eql("elf_time", k.elf_time) && eqs("func", k.func) && eqs("out_abs", k.out_abs.string()) && eqi("sm", k.sm) &&
         eqi("require_known", k.require_known ? 1 : 0) && eqi("bundle_regext", k.bundle_regext ? 1 : 0) && eqi("include_comments", k.include_comments ? 1 : 0) &&
         eqi("scalar_leader_only", k.scalar_leader_only ? 1 : 0) &&
         eqs("encoding_h_abs", k.encoding_h_abs) && eql("encoding_time", k.encoding_time) && eqs("exe_abs", k.exe_abs) && eql("exe_time", k.exe_time);
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
  fs::path encoding_h = "ventus-env/spike/riscv/encoding.h";
  bool require_known = false;
  bool bundle_regext = true;
  bool include_comments = true;
  int sm = 75;
  bool cache_enabled = true;

  elf_path = argv[1];

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--func" && i + 1 < argc) {
      func = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--encoding-h" && i + 1 < argc) {
      encoding_h = argv[++i];
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
  if (out_path.empty()) out_path = default_out_path(elf_path, *func);

  try {
    const auto profile_log = env_str("GPU_SBT_PTX_PROFILE_LOG");
    const bool profile = profile_log.has_value();
    const auto t0_total = std::chrono::steady_clock::now();

    if (env_truthy("GPU_SBT_PTX_NO_CACHE")) cache_enabled = false;

    CacheKey key;
    key.elf_abs = fs::absolute(elf_path).string();
    key.elf_time = file_time_token(elf_path);
    key.func = *func;
    key.out_abs = fs::absolute(out_path);
    key.sm = sm;
    key.require_known = require_known;
    key.bundle_regext = bundle_regext;
    key.include_comments = include_comments;
    key.scalar_leader_only = env_truthy("GPU_SBT_SCALAR_LEADER_ONLY");
    key.encoding_h_abs = fs::absolute(encoding_h).string();
    key.encoding_time = file_time_token(encoding_h);
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
      const bool out_ok = fs::exists(out_abs, ec) && fs::is_regular_file(out_abs, ec) && (fs::file_size(out_abs, ec) > 0);
      const bool meta_ok = fs::exists(meta_path, ec) && fs::is_regular_file(meta_path, ec);
      if (out_ok && meta_ok) {
        if (auto meta = read_text_file(meta_path)) {
          if (cache_meta_matches(key, *meta)) {
            cache_hit = true;
            const auto t1_total = std::chrono::steady_clock::now();
            t_total_ms =
                std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1_total - t0_total).count();
            if (profile) {
              std::ostringstream j;
              j << "{\"ts_ms\":" << (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
              j << ",\"pid\":" << (long long)::getpid();
              j << ",\"elf\":\"" << json_escape(key.elf_abs) << "\"";
              j << ",\"func\":\"" << json_escape(key.func) << "\"";
              j << ",\"out\":\"" << json_escape(key.out_abs.string()) << "\"";
              j << ",\"sm\":" << key.sm;
              j << ",\"scalar_leader_only\":" << (key.scalar_leader_only ? "true" : "false");
              j << ",\"cache_hit\":true";
              j << ",\"t_total_ms\":" << t_total_ms;
              j << "}";
              append_line_atomic(*profile_log, j.str());
            }
            // Keep stdout compatible with existing driver usage.
            std::cout << "已生成 PTX: " << out_path << "\n";
            std::cout << "提示: 需要 dynamic shared >= warps_per_block*1024(wctx) + warps_per_block*1024(stack) + ldsSize（launch 时设置）\n";
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
      if (!s.name.empty()) sym_by_addr.emplace(s.addr, s.name);
    }

    const uint32_t text_end = text.vaddr + static_cast<uint32_t>(text.data.size());
    const auto fr = find_func_range(syms, text.vaddr, text_end, *func);
    if (!fr) {
      std::cerr << "未找到函数符号或不在 .text: " << *func << "\n";
      return 1;
    }

    if (fr->start < text.vaddr || fr->end > text_end || fr->end <= fr->start) {
      std::cerr << "函数范围非法: " << *func << " start=" << hex_u32(fr->start) << " end=" << hex_u32(fr->end) << "\n";
      return 1;
    }

    const size_t off = fr->start - text.vaddr;
    const size_t len = fr->end - fr->start;
    std::vector<uint8_t> slice(text.data.begin() + static_cast<long>(off), text.data.begin() + static_cast<long>(off + len));

    const auto pack = build_patterns_from_encoding(encoding_h);

    sbt::DecodeOptions dopt;
    dopt.bundle_regext = bundle_regext;
    dopt.require_known = require_known;
    const auto decoded = sbt::decode_text(slice, fr->start, dopt, pack.patterns);

    const auto cfg = sbt::cfg::build_function_cfg(decoded, fr->start, fr->end);
    const auto verify = sbt::cfg::verify_function(cfg, *func);

    const bool vbranch_ok = std::all_of(verify.vbranch.begin(), verify.vbranch.end(), [](const sbt::cfg::VBranchCheck &c) { return c.error.empty(); });
    const bool barrier_ok = std::all_of(verify.barriers.begin(), verify.barriers.end(), [](const sbt::cfg::BarrierCheck &c) { return c.ok; });
    const bool jalr_ok = verify.unsupported_jalr.empty();
    if (!vbranch_ok || !barrier_ok || !jalr_ok) {
      std::cerr << "CFG 结构化验证未通过: " << *func << "\n";
      std::cerr << "  vbranch_ok=" << (vbranch_ok ? "true" : "false") << " barrier_ok=" << (barrier_ok ? "true" : "false")
                << " jalr_ok=" << (jalr_ok ? "true" : "false") << "\n";
      return 2;
    }

    sbt::ptx::Options popt;
    popt.sm = sm;
    popt.include_comments = include_comments;
    popt.scalar_exec_leader_only = env_truthy("GPU_SBT_SCALAR_LEADER_ONLY");
    if (const char *v = std::getenv("GPU_SBT_PDS_BYTES")) {
      try {
        unsigned long n = std::stoul(std::string(v));
        if (n < 4 || n > (1u << 20)) {
          throw std::runtime_error("out of range");
        }
        // Align down to 4 bytes to match u32 local accesses.
        n &= ~3ul;
        if (n < 4) n = 4;
        popt.pds_bytes = static_cast<uint32_t>(n);
      } catch (...) {
        std::cerr << "GPU_SBT_PDS_BYTES 解析失败: '" << v << "' (expect integer bytes)\n";
        return 2;
      }
    }

    const auto res = sbt::ptx::emit_kernel(cfg, sym_by_addr, *func, popt);

    const auto t1_work = std::chrono::steady_clock::now();
    t_work_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1_work - t0_work).count();

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
    t_write_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1_write - t0_write).count();

    if (cache_enabled) {
      const fs::path out_abs = key.out_abs;
      const fs::path meta_path = cache_meta_path_for(out_abs);
      const fs::path tmp = meta_path.string() + ".tmp." + std::to_string(::getpid());
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
    t_total_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1_total - t0_total).count();

    if (profile) {
      std::ostringstream j;
      j << "{\"ts_ms\":" << (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      j << ",\"pid\":" << (long long)::getpid();
      j << ",\"elf\":\"" << json_escape(key.elf_abs) << "\"";
      j << ",\"func\":\"" << json_escape(key.func) << "\"";
      j << ",\"out\":\"" << json_escape(key.out_abs.string()) << "\"";
      j << ",\"sm\":" << key.sm;
      j << ",\"scalar_leader_only\":" << (key.scalar_leader_only ? "true" : "false");
      j << ",\"cache_hit\":false";
      j << ",\"t_total_ms\":" << t_total_ms;
      j << ",\"t_work_ms\":" << t_work_ms;
      j << ",\"t_write_ms\":" << t_write_ms;
      j << "}";
      append_line_atomic(*profile_log, j.str());
    }

    std::cout << "已生成 PTX: " << out_path << "\n";
    std::cout << "提示: 需要 dynamic shared >= warps_per_block*1024(wctx) + warps_per_block*1024(stack) + ldsSize（launch 时设置）\n";
    return 0;
  } catch (const sbt::ptx::EmitError &e) {
    std::cerr << "PTX 生成失败: " << e.what() << "\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << "\n";
    return 1;
  }
}
