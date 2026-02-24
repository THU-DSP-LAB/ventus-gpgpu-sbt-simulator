#include "sbt/want_file.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace sbt {
namespace fs = std::filesystem;
namespace {

static std::optional<std::string> getenv_str(const char *name) {
  if (name == nullptr) return std::nullopt;
  const char *v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') return std::nullopt;
  return std::string(v);
}

static std::optional<fs::path> self_exe_path() {
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return std::nullopt;
  return p;
}

static bool is_repo_root(const fs::path &p) {
  return fs::exists(p / "CMakeLists.txt") && fs::exists(p / "VentusInst_basic.txt");
}

static std::optional<fs::path> find_repo_root_from_exe() {
  const auto exe = self_exe_path();
  if (!exe) return std::nullopt;
  fs::path p = exe->parent_path();
  for (int i = 0; i < 16; ++i) {
    if (is_repo_root(p)) return p;
    if (!p.has_parent_path()) break;
    p = p.parent_path();
  }
  return std::nullopt;
}

static void trim_in_place(std::string &s) {
  auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t b = 0;
  while (b < s.size() && is_ws(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
  s = s.substr(b, e - b);
}

} // namespace

fs::path resolve_spike_want_file() {
  if (const auto v = getenv_str("GPU_SBT_WANT_FILE")) return fs::path(*v);
  if (const auto root = find_repo_root_from_exe()) {
    const fs::path p = *root / "data" / "spike_want.txt";
    if (fs::exists(p)) return p;
  }
  return fs::path("data/spike_want.txt");
}

WantList load_spike_want_list(const fs::path &path) {
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

