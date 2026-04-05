#pragma once

#include "sbt/riscv_decode.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sbt::cfg {

enum class EdgeKind : uint8_t {
  Fallthrough = 0,
  Branch = 1,
  Jump = 2,
};

const char *to_string(EdgeKind k);

struct Edge final {
  uint32_t src = 0; // block start
  uint32_t dst = 0; // block start
  EdgeKind kind = EdgeKind::Fallthrough;
};

struct BundleInst final {
  uint32_t pc = 0;      // bundle start PC (includes regext prefix if present)
  uint32_t inst_pc = 0; // actual instruction PC
  uint8_t len = 4;      // bytes: 4 + bundled prefix bytes (usually 4/8, compat chains may be 12+)
  DecodedInst inst{};
};

struct BasicBlock final {
  uint32_t start = 0;
  std::vector<size_t> inst_indices;
  std::vector<Edge> succs;
};

struct FunctionCfg final {
  uint32_t start = 0;
  uint32_t end = 0; // exclusive

  std::vector<BundleInst> insts;
  std::vector<BasicBlock> blocks;

  std::unordered_map<uint32_t, size_t> inst_index_by_pc;     // bundle pc -> inst index
  std::unordered_map<uint32_t, uint32_t> inst_pc_to_block;   // bundle pc -> block start
  std::unordered_map<uint32_t, size_t> block_index_by_start; // block start -> index
};

FunctionCfg
build_function_cfg(const std::vector<DecodedInst> &decoded, uint32_t func_start, uint32_t func_end_excl);

} // namespace sbt::cfg
