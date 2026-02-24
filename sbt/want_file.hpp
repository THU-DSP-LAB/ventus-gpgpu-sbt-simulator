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

// Resolve the default Spike whitelist file path:
// - if GPU_SBT_WANT_FILE is set, use it
// - else try to locate the repo root from the current executable path
// - else fall back to "data/spike_want.txt" relative to CWD
std::filesystem::path resolve_spike_want_file();

// Load a want file from disk (one id per line; supports '#' comments).
// Throws std::runtime_error on IO/parse errors.
WantList load_spike_want_list(const std::filesystem::path &path);

} // namespace sbt

