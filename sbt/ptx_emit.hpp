#pragma once

#include "sbt/cfg.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace sbt::ptx {

struct Options final {
  // Default to the lowest arch supported by modern CUDA toolchains (e.g. ptxas in CUDA 13.x).
  int sm = 75;

  // Ventus numeric address space layout (prototype defaults).
  uint32_t shared_base_vaddr = 0x7000'0000u;
  uint32_t elf_base_vaddr = 0x8000'0000u;
  uint32_t heap_base_vaddr = 0x9000'0000u;

  // Per-warp scalar stack inside shared segment: x2 = shared_base + warp_id * stride.
  uint32_t stack_stride_bytes = 1024u;

  // Bytes reserved for Ventus local/shared segment in dynamic shared memory.
  uint32_t lds_bytes = 32u * 1024u;

  // Per-thread private backing store size (bytes) for `vlw.v`/`vsw.v` when CSR_PDS is unavailable.
  // This becomes a PTX `.local` array and directly impacts stack frame size.
  uint32_t pds_bytes = 4096u;

  // Execute scalar (x-reg) instructions only on the active-lane leader and store x-reg state once per warp.
  // This matches the "per-warp scalar regfile in shared" model and avoids redundant shared traffic.
  bool scalar_exec_leader_only = false;

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

EmitResult emit_kernel(const sbt::cfg::FunctionCfg &cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &kernel_name, const Options &opt);

} // namespace sbt::ptx
