#include "sbt/dump_parser.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sbt::dump {
namespace {

static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t parse_hex_u32(std::string_view s) {
  uint64_t v = 0;
  std::stringstream ss;
  ss << std::hex << s;
  ss >> v;
  return static_cast<uint32_t>(v & 0xFFFF'FFFFu);
}

static uint8_t parse_hex_u8(std::string_view s) {
  uint64_t v = 0;
  std::stringstream ss;
  ss << std::hex << s;
  ss >> v;
  return static_cast<uint8_t>(v & 0xFFu);
}

} // namespace

std::unordered_map<uint32_t, DumpInst> parse_dump(const std::filesystem::path &dump_path) {
  std::ifstream f(dump_path);
  if (!f) throw std::runtime_error("cannot open: " + dump_path.string());

  std::unordered_map<uint32_t, DumpInst> out;
  std::string line;
  while (std::getline(f, line)) {
    // Fast check: instruction lines start with hex address and ':'.
    if (line.size() < 10) continue;
    if (!is_hex(line[0])) continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;

    const std::string_view pc_str(line.data(), colon);
    uint32_t pc = 0;
    try {
      pc = parse_hex_u32(pc_str);
    } catch (...) {
      continue;
    }

    std::string rest = line.substr(colon + 1);
    std::istringstream iss(rest);
    std::string b0, b1, b2, b3;
    if (!(iss >> b0 >> b1 >> b2 >> b3)) continue;
    if (b0.size() != 2 || b1.size() != 2 || b2.size() != 2 || b3.size() != 2) continue;

    DumpInst di;
    di.pc = pc;
    di.bytes = {parse_hex_u8(b0), parse_hex_u8(b1), parse_hex_u8(b2), parse_hex_u8(b3)};
    out.emplace(pc, di);
  }
  return out;
}

} // namespace sbt::dump

