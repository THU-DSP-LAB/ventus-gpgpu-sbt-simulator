#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>

namespace sbt::dump {

struct DumpInst final {
  uint32_t pc = 0;
  std::array<uint8_t, 4> bytes{};
};

std::unordered_map<uint32_t, DumpInst> parse_dump(const std::filesystem::path &dump_path);

} // namespace sbt::dump

