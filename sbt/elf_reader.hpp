#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sbt::elf {

class ElfError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ElfSection final {
  std::string name;
  uint32_t vaddr = 0;
  std::vector<uint8_t> data;
};

struct FuncSymbol final {
  std::string name;
  uint32_t addr = 0;
  uint32_t size = 0;
  uint8_t bind = 0;
  uint8_t type = 0;
  uint8_t vis = 0;
  uint16_t shndx = 0;
};

ElfSection read_section(const std::filesystem::path &elf_path, std::string_view section_name);

std::vector<FuncSymbol> read_func_symbols(const std::filesystem::path &elf_path);

} // namespace sbt::elf

