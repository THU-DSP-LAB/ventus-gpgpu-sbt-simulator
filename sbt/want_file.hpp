#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace sbt {

struct WantList final {
  std::vector<std::string> ids;         // preserves file order
  std::unordered_set<std::string> set;  // for fast lookup
};

// Load a want file from disk (one id per line; supports '#' comments).
// Throws std::runtime_error on IO/parse errors.
WantList load_spike_want_list(const std::filesystem::path &path);

} // namespace sbt
