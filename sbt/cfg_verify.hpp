#pragma once

#include "sbt/cfg.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sbt::cfg {

class VRegUniformFacts final {
public:
  static constexpr int kMaxVReg = 256;

  static VRegUniformFacts empty() { return VRegUniformFacts(false); }
  static VRegUniformFacts full() { return VRegUniformFacts(true); }

  bool test(int r) const {
    if (r < 0 || r >= kMaxVReg)
      return false;
    const size_t wi = static_cast<size_t>(r) / 64;
    const size_t bi = static_cast<size_t>(r) % 64;
    return (words_[wi] >> bi) & 1ULL;
  }

  void set(int r, bool v) {
    if (r < 0 || r >= kMaxVReg)
      return;
    const size_t wi = static_cast<size_t>(r) / 64;
    const size_t bi = static_cast<size_t>(r) % 64;
    if (v)
      words_[wi] |= (1ULL << bi);
    else
      words_[wi] &= ~(1ULL << bi);
  }

  void clear_all() { words_.fill(0ULL); }

  VRegUniformFacts &operator&=(const VRegUniformFacts &o) {
    for (size_t i = 0; i < words_.size(); ++i)
      words_[i] &= o.words_[i];
    return *this;
  }

  bool operator==(const VRegUniformFacts &o) const {
    return words_ == o.words_;
  }
  bool operator!=(const VRegUniformFacts &o) const { return !(*this == o); }

private:
  explicit VRegUniformFacts(bool fill) {
    if (fill)
      words_.fill(~0ULL);
    else
      words_.fill(0ULL);
  }

  std::array<uint64_t, 4> words_{};
};

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
  VRegUniformFacts entry_uniform_vregs = VRegUniformFacts::empty();
  bool entry_converged = true;
};

struct DirectCallFacts final {
  uint32_t call_addr = 0;
  uint32_t call_block = 0;
  uint32_t callee_addr = 0;
  VRegUniformFacts pre_call_uniform_vregs = VRegUniformFacts::empty();
  bool call_context_converged = true;
};

FunctionVerifyResult verify_function(const FunctionCfg &cfg,
                                     std::string func_name,
                                     const VerifyOptions &options);
FunctionVerifyResult verify_function(const FunctionCfg &cfg, std::string func_name);

std::vector<DirectCallFacts>
collect_direct_call_facts(const FunctionCfg &cfg, const VerifyOptions &options);

} // namespace sbt::cfg
