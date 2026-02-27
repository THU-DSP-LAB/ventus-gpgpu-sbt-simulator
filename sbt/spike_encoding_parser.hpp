#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace sbt::spike {

struct InsnPattern final {
  std::string name;
  uint32_t match = 0;
  uint32_t mask = 0;
};

// Parse Spike's `encoding.h` and return all `DECLARE_INSN(name, MATCH_*, MASK_*)`.
//
// Notes:
// - This is a text parser: it does not run the preprocessor.
// - It is intended for bring-up and tooling only.
std::unordered_map<std::string, InsnPattern>
parse_declared_insns(const std::filesystem::path &encoding_h_path);

} // namespace sbt::spike
