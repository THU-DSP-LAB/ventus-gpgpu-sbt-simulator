#include "sbt/cfg.hpp"
#include "sbt/cfg_verify.hpp"
#include "sbt/elf_reader.hpp"
#include "sbt/ptx_emit.hpp"
#include "sbt/riscv_decode.hpp"
#include "sbt/spike_encoding_parser.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

static void usage() {
  std::cerr << "用法:\n";
  std::cerr << "  sbt_ptx <elf> --func <kernel> [--out <ptx>] [--sm <cc>] [--encoding-h <path>]\n";
  std::cerr << "          [--require-known] [--no-bundle-regext] [--no-comments]\n";
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
      "vsub12_vi",  "vid_v",     "vmv_v_x",    "vsetvli",   "vadd_vv",   "vadd_vx",
      "vadd_vi",    "vsub_vv",   "vand_vv",    "vor_vv",    "vxor_vi",   "vsll_vi",
      "vsrl_vi",    "vsra_vi",   "vmul_vx",    "vdivu_vx",  "vremu_vx",  "vmadd_vv",
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

    const auto res = sbt::ptx::emit_kernel(cfg, sym_by_addr, *func, popt);

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
