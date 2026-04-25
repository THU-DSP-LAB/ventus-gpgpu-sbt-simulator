#pragma once

#include "sbt/cfg.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sbt::control {

struct Semantics final {
  EmitDomain domain = EmitDomain::None;
  ControlKind control_kind = ControlKind::None;
  StructuredControlKind structured_control_kind = StructuredControlKind::None;
  BranchCondKind branch_cond = BranchCondKind::None;

  bool is_scalar_branch = false;
  bool is_vector_branch = false;
  bool is_direct_jump = false;
  bool is_direct_call = false;
  bool is_return = false;
  bool is_indirect_terminator = false;

  bool is_setrpc = false;
  bool is_join = false;
  bool is_barrier = false;
  bool is_vsetvli = false;
  bool is_endprg = false;

  bool is_terminator = false;
  bool has_fallthrough = true;
  bool has_direct_target = false;
  int64_t direct_target = 0;
};

Semantics classify(const cfg::BundleInst &bi);
bool is_scalar_auipc(const DecodedInst &di);
std::optional<uint32_t> resolve_setrpc_join_pc(const cfg::FunctionCfg &cfg,
                                               size_t setrpc_inst_idx);
std::vector<uint32_t>
collect_direct_call_targets(const cfg::FunctionCfg &cfg,
                            std::string_view owner_name = {});

} // namespace sbt::control
