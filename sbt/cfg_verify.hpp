#pragma once

#include "sbt/cfg.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sbt::cfg {

struct VBranchCheck final {
  uint32_t vbranch_addr = 0;
  uint32_t vbranch_block = 0;
  std::string mnemonic;

  std::optional<uint32_t> target;
  std::optional<uint32_t> fallthrough;
  std::optional<uint32_t> join_pc;

  bool proven_uniform = false;
  bool join_is_join_inst = false;
  bool loop_like = false;
  bool postdom_ok = false;
  bool no_side_exit_ok = false;
  bool single_entry_ok = false;

  std::string error;
};

struct BarrierCheck final {
  uint32_t barrier_addr = 0;
  uint32_t barrier_block = 0;
  bool ok = false;
  std::string error;
};

struct UnsupportedJalr final {
  uint32_t addr = 0;
  uint32_t word = 0;
};

struct FunctionVerifyResult final {
  std::string func;
  uint32_t start = 0;
  uint32_t end = 0;
  size_t insts = 0;
  size_t blocks = 0;
  size_t edges = 0;

  std::vector<VBranchCheck> vbranch;
  std::vector<BarrierCheck> barriers;
  std::vector<UnsupportedJalr> unsupported_jalr;
};

struct VerifyOptions final {
  const std::unordered_map<uint32_t, std::string> *sym_by_addr = nullptr;
};

FunctionVerifyResult verify_function(const FunctionCfg &cfg,
                                     std::string func_name,
                                     const VerifyOptions &options);
FunctionVerifyResult verify_function(const FunctionCfg &cfg, std::string func_name);

} // namespace sbt::cfg
