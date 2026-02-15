#include "sbt/spike_encoding_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

static void usage() {
  std::cerr << "用法: gen_spike_encoding_subset --encoding-h <path> --out <path>\n";
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path encoding_h = "ventus-env/spike/riscv/encoding.h";
  std::filesystem::path out = "sbt/generated/spike_encoding_subset.hpp";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--encoding-h" && i + 1 < argc) {
      encoding_h = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out = argv[++i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "未知参数: " << a << "\n";
      usage();
      return 2;
    }
  }

  const std::unordered_map<std::string, sbt::spike::InsnPattern> insns = sbt::spike::parse_declared_insns(encoding_h);

  // Rodinia bring-up whitelist (instruction IDs in encoding.h, using '_' naming).
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

  std::vector<sbt::spike::InsnPattern> subset;
  subset.reserve(want.size());
  for (const auto &n : want) {
    auto it = insns.find(n);
    if (it == insns.end()) {
      std::cerr << "未找到 DECLARE_INSN: " << n << "\n";
      return 1;
    }
    subset.push_back(it->second);
  }

  std::filesystem::create_directories(out.parent_path());
  std::ofstream f(out);
  if (!f) {
    std::cerr << "无法写入: " << out << "\n";
    return 1;
  }

  f << "#pragma once\n\n";
  f << "// Auto-generated from spike encoding.h (subset for Rodinia bring-up)\n";
  f << "// Source: " << encoding_h.string() << "\n\n";
  f << "#include <cstdint>\n\n";
  f << "namespace sbt::gen {\n";
  f << "struct Pattern final { const char* name; uint32_t match; uint32_t mask; };\n";
  f << "inline constexpr Pattern kPatterns[] = {\n";
  for (const auto &p : subset) {
    f << "  {\"" << p.name << "\", 0x" << std::hex << p.match << "u, 0x" << p.mask << "u},\n";
  }
  f << "};\n";
  f << "} // namespace sbt::gen\n";

  return 0;
}
