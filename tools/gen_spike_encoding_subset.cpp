#include "sbt/spike_encoding_parser.hpp"
#include "sbt/want_file.hpp"

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
  std::filesystem::path encoding_h = "../spike/riscv/encoding.h";
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

  const sbt::WantList want = sbt::load_spike_want_list(sbt::resolve_spike_want_file());

  std::vector<sbt::spike::InsnPattern> subset;
  subset.reserve(want.ids.size());
  for (const auto &id : want.ids) {
    auto it = insns.find(id);
    if (it == insns.end()) {
      std::cerr << "未找到 DECLARE_INSN: " << id << "\n";
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
