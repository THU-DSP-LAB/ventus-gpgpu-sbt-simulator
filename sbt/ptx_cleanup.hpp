#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace sbt::ptx::detail {

// Operates on the emitter's function body, preserving opaque PTX operations.
std::string cleanup_ptx_body(std::string_view body);
std::unordered_set<std::string> ptx_registers(std::string_view text);

} // namespace sbt::ptx::detail
