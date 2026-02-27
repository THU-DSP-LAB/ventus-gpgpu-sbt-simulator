#include "sbt/want_file.hpp"

#include <fstream>
#include <stdexcept>

namespace sbt {
namespace {

static void trim_in_place(std::string &s) {
  auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t b = 0;
  while (b < s.size() && is_ws(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
  s = s.substr(b, e - b);
}

} // namespace

WantList load_spike_want_list(const std::filesystem::path &path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("无法读取 want 文件: " + path.string());

  WantList out;
  std::string line;
  while (std::getline(f, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    trim_in_place(line);
    if (line.empty()) continue;

    if (out.set.insert(line).second) {
      out.ids.push_back(line);
    }
  }

  if (out.ids.empty()) throw std::runtime_error("want 文件为空: " + path.string());
  return out;
}

} // namespace sbt
