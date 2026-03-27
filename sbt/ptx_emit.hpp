#pragma once

#include "sbt/cfg.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sbt::ptx {

struct Options final {
  // Default to the lowest arch supported by modern CUDA toolchains (e.g. ptxas in CUDA 13.x).
  int sm = 75;

  // Ventus numeric address space layout (prototype defaults).
  uint32_t shared_base_vaddr = 0x7000'0000u;
  uint32_t global_base_vaddr = 0x8000'0000u;

  // Per-warp scalar stack inside shared segment: x2 = shared_base + warp_id * stride.
  uint32_t stack_stride_bytes = 1024u;

  // Bytes reserved for Ventus local/shared segment in dynamic shared memory.
  uint32_t lds_bytes = 32u * 1024u;

  bool include_comments = true;
};

struct EmitError final : public std::runtime_error {
  std::string code;
  std::string func;
  uint32_t pc = 0;

  EmitError(std::string code_, std::string func_, uint32_t pc_, std::string detail);
};

struct EmitResult final {
  std::string ptx;
};

struct FuncToEmit final {
  // Ventus symbol name (for error messages / comments).
  std::string name;
  // PTX function symbol name to emit/call.
  std::string ptx_name;
  // Function CFG to emit.
  sbt::cfg::FunctionCfg cfg;
};

// Emit a PTX module containing one `.entry` kernel plus any number of `.func` callees.
//
// - `ptx_name_by_addr` maps Ventus call targets (function entry PC) to PTX `.func` symbols.
// - Calls to `is_inlined_builtin_call_name()` are still inlined; only other direct calls use this map.
EmitResult emit_module(const sbt::cfg::FunctionCfg &entry_cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &entry_name, const std::vector<FuncToEmit> &funcs,
                       const std::unordered_map<uint32_t, std::string> &ptx_name_by_addr, const Options &opt);

// Builtins that are always inlined/handled specially by the PTX emitter (prototype).
bool is_inlined_builtin_call_name(std::string_view callee);

EmitResult emit_kernel(const sbt::cfg::FunctionCfg &cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &kernel_name, const Options &opt);

} // namespace sbt::ptx
