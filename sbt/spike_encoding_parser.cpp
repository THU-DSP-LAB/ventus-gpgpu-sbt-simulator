#include "sbt/spike_encoding_parser.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sbt::spike {
namespace {

static std::string trim(std::string s) {
  auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

static bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

static uint32_t parse_u32(std::string_view v) {
  // Accept "0x..." only.
  if (!starts_with(v, "0x")) throw std::runtime_error("expected hex literal: " + std::string(v));
  uint64_t x = 0;
  std::stringstream ss;
  ss << std::hex << v.substr(2);
  ss >> x;
  return static_cast<uint32_t>(x & 0xFFFF'FFFFu);
}

static bool parse_define_match_mask(
    const std::string &line,
    std::unordered_map<std::string, uint32_t> &match_by_suffix,
    std::unordered_map<std::string, uint32_t> &mask_by_suffix
) {
  // Example: "#define MATCH_VLW12_V 0x207b"
  std::istringstream iss(line);
  std::string hash_define;
  std::string macro;
  std::string value;
  if (!(iss >> hash_define >> macro >> value)) return false;
  if (hash_define != "#define") return false;
  if (starts_with(macro, "MATCH_")) {
    match_by_suffix[macro.substr(6)] = parse_u32(value);
    return true;
  }
  if (starts_with(macro, "MASK_")) {
    mask_by_suffix[macro.substr(5)] = parse_u32(value);
    return true;
  }
  return false;
}

static bool parse_declare_insn(
    const std::string &line,
    std::unordered_map<std::string, std::pair<std::string, std::string>> &decl
) {
  // Example: "DECLARE_INSN(vlw12_v, MATCH_VLW12_V, MASK_VLW12_V)"
  const std::string key = "DECLARE_INSN(";
  const auto pos = line.find(key);
  if (pos == std::string::npos) return false;
  const auto l = pos + key.size();
  const auto r = line.find(')', l);
  if (r == std::string::npos) return false;
  const std::string inside = line.substr(l, r - l);

  std::string a, b, c;
  std::stringstream ss(inside);
  if (!std::getline(ss, a, ',')) return false;
  if (!std::getline(ss, b, ',')) return false;
  if (!std::getline(ss, c, ',')) return false;

  a = trim(a);
  b = trim(b);
  c = trim(c);

  // b/c are expected to be MATCH_*/MASK_*
  if (!starts_with(b, "MATCH_")) return false;
  if (!starts_with(c, "MASK_")) return false;
  decl[a] = {b.substr(6), c.substr(5)};
  return true;
}

} // namespace

std::unordered_map<std::string, InsnPattern>
parse_declared_insns(const std::filesystem::path &encoding_h_path) {
  std::ifstream f(encoding_h_path);
  if (!f) throw std::runtime_error("cannot open: " + encoding_h_path.string());

  std::unordered_map<std::string, uint32_t> match_by_suffix;
  std::unordered_map<std::string, uint32_t> mask_by_suffix;
  std::unordered_map<std::string, std::pair<std::string, std::string>> declared;

  std::string line;
  while (std::getline(f, line)) {
    (void)parse_define_match_mask(line, match_by_suffix, mask_by_suffix);
    (void)parse_declare_insn(line, declared);
  }

  std::unordered_map<std::string, InsnPattern> out;
  out.reserve(declared.size());
  for (const auto &kv : declared) {
    const std::string &insn_name = kv.first;
    const std::string &match_suf = kv.second.first;
    const std::string &mask_suf = kv.second.second;

    auto it_m = match_by_suffix.find(match_suf);
    auto it_k = mask_by_suffix.find(mask_suf);
    if (it_m == match_by_suffix.end() || it_k == mask_by_suffix.end()) continue;

    InsnPattern p;
    p.name = insn_name;
    p.match = it_m->second;
    p.mask = it_k->second;
    out.emplace(insn_name, std::move(p));
  }
  return out;
}

} // namespace sbt::spike

