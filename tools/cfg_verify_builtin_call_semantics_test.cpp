/*
背景
- CFG verifier 需要在判断 vbranch/barrier 合法性前理解 direct-call builtin helper 对 vector register uniformity 的影响。
- PTX emitter 会内联一组 OpenCL/Ventus builtin helper；这些 helper 的返回值不能让调用前的 stale vector-uniform fact 静默穿透。

需求/作用
- 回归验证 verifier 对 builtin helper call summary 的处理。
- 覆盖 no-summary direct call 的保守边界，以及 shared builtin lookup / summary / public classifier 的 drift 防护。

用法
- 构建后运行：`timeout 60s build/cfg_verify_builtin_call_semantics_test`

实现原理/处理步骤
- 手工构造最小 `FunctionCfg`，用 direct `jal` 指向指定符号名模拟 builtin/helper call。
- 调用 `verify_function(..., VerifyOptions{sym_by_addr})`，检查 vbranch uniform proof 与 barrier 诊断。
- 同时遍历 shared builtin entries / summaries，确认 emitter public classifier 与 verifier summary 同步。
*/

#include "sbt/builtin_semantics.hpp"
#include "sbt/cfg_verify.hpp"
#include "sbt/ptx_emit.hpp"
#include "sbt/riscv_decode.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error("assert: " + msg);
}

sbt::cfg::BundleInst make_inst(uint32_t pc, const std::string &name) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = name;
  (void)sbt::populate_inst_metadata(name, bi.inst);
  sbt::finalize_emit_descriptor(bi.inst);
  if (bi.inst.inst_id == sbt::kUnknownInstId && name != "unknown")
    bi.inst.inst_id = sbt::make_inst_id(name);
  return bi;
}

sbt::cfg::BundleInst make_auipc(uint32_t pc, int rd) {
  auto bi = make_inst(pc, "auipc");
  bi.inst.rd = rd;
  bi.inst.imm = 0;
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::BundleInst make_setrpc(uint32_t pc, int rs1, uint32_t auipc_pc,
                                 uint32_t join_pc) {
  auto bi = make_inst(pc, "setrpc");
  bi.inst.rs1 = rs1;
  bi.inst.imm = static_cast<int32_t>(join_pc - auipc_pc);
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::BundleInst make_vmv_i(uint32_t pc, int rd, int imm) {
  auto bi = make_inst(pc, "vmv_v_i");
  bi.inst.rd = rd;
  bi.inst.imm = imm;
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::BundleInst make_call(uint32_t pc, uint32_t target) {
  auto bi = make_inst(pc, "jal");
  bi.inst.rd = 1;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::BundleInst make_jump(uint32_t pc, uint32_t target) {
  auto bi = make_inst(pc, "jal");
  bi.inst.rd = 0;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::BundleInst make_vbranch(uint32_t pc, uint32_t target) {
  auto bi = make_inst(pc, "vbeq");
  bi.inst.rs1 = 0;
  bi.inst.rs2 = 0;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

void append_block(sbt::cfg::FunctionCfg &cfg, uint32_t start,
                  std::vector<size_t> inst_indices,
                  std::vector<sbt::cfg::Edge> succs) {
  const size_t block_idx = cfg.blocks.size();
  cfg.block_index_by_start.emplace(start, block_idx);
  for (size_t inst_idx : inst_indices) {
    cfg.inst_pc_to_block.emplace(cfg.insts.at(inst_idx).pc, start);
  }
  cfg.blocks.push_back(sbt::cfg::BasicBlock{start, std::move(inst_indices),
                                            std::move(succs)});
}

struct Scenario final {
  sbt::cfg::FunctionCfg cfg;
  std::unordered_map<uint32_t, std::string> sym_by_addr;
};

Scenario make_single_call_branch_cfg(uint32_t start, uint32_t callee_pc,
                                     const std::string &callee,
                                     bool include_barrier,
                                     bool include_symbol) {
  const uint32_t join_pc = start + 0x80u;
  const uint32_t branch_target = start + 0x60u;
  const uint32_t else_pc = start + 0x14u;
  const uint32_t else_jump_pc = include_barrier ? start + 0x18u : else_pc;

  Scenario s;
  auto &cfg = s.cfg;
  cfg.start = start;
  cfg.end = join_pc + 8u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, start, join_pc),
      make_vmv_i(start + 8u, 0, 0),
      make_call(start + 0x0cu, callee_pc),
      make_vbranch(start + 0x10u, branch_target),
  };
  if (include_barrier)
    cfg.insts.push_back(make_inst(else_pc, "barrier"));
  cfg.insts.push_back(make_jump(else_jump_pc, join_pc));
  const size_t then_jump_idx = cfg.insts.size();
  cfg.insts.push_back(make_jump(branch_target, join_pc));
  const size_t join_idx = cfg.insts.size();
  cfg.insts.push_back(make_inst(join_pc, "join"));
  cfg.insts.push_back(make_inst(join_pc + 4u, "endprg"));

  for (size_t i = 0; i < cfg.insts.size(); ++i)
    cfg.inst_index_by_pc.emplace(cfg.insts[i].pc, i);

  append_block(cfg, start, {0, 1, 2, 3, 4},
               {sbt::cfg::Edge{start, branch_target, sbt::cfg::EdgeKind::Branch},
                sbt::cfg::Edge{start, else_pc, sbt::cfg::EdgeKind::Fallthrough}});
  if (include_barrier) {
    append_block(cfg, else_pc, {5, 6},
                 {sbt::cfg::Edge{else_pc, join_pc, sbt::cfg::EdgeKind::Jump}});
  } else {
    append_block(cfg, else_pc, {5},
                 {sbt::cfg::Edge{else_pc, join_pc, sbt::cfg::EdgeKind::Jump}});
  }
  append_block(cfg, branch_target, {then_jump_idx},
               {sbt::cfg::Edge{branch_target, join_pc, sbt::cfg::EdgeKind::Jump}});
  append_block(cfg, join_pc, {join_idx, join_idx + 1}, {});

  if (include_symbol)
    s.sym_by_addr.emplace(callee_pc, callee);
  return s;
}

Scenario make_two_call_branch_cfg(uint32_t start, uint32_t first_callee_pc,
                                  const std::string &first_callee,
                                  uint32_t second_callee_pc,
                                  const std::string &second_callee) {
  const uint32_t join_pc = start + 0x80u;
  const uint32_t branch_target = start + 0x60u;
  const uint32_t else_pc = start + 0x18u;

  Scenario s;
  auto &cfg = s.cfg;
  cfg.start = start;
  cfg.end = join_pc + 8u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, start, join_pc),
      make_vmv_i(start + 8u, 0, 0),
      make_call(start + 0x0cu, first_callee_pc),
      make_call(start + 0x10u, second_callee_pc),
      make_vbranch(start + 0x14u, branch_target),
      make_jump(else_pc, join_pc),
      make_jump(branch_target, join_pc),
      make_inst(join_pc, "join"),
      make_inst(join_pc + 4u, "endprg"),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i)
    cfg.inst_index_by_pc.emplace(cfg.insts[i].pc, i);

  append_block(cfg, start, {0, 1, 2, 3, 4, 5},
               {sbt::cfg::Edge{start, branch_target, sbt::cfg::EdgeKind::Branch},
                sbt::cfg::Edge{start, else_pc, sbt::cfg::EdgeKind::Fallthrough}});
  append_block(cfg, else_pc, {6},
               {sbt::cfg::Edge{else_pc, join_pc, sbt::cfg::EdgeKind::Jump}});
  append_block(cfg, branch_target, {7},
               {sbt::cfg::Edge{branch_target, join_pc, sbt::cfg::EdgeKind::Jump}});
  append_block(cfg, join_pc, {8, 9}, {});

  s.sym_by_addr.emplace(first_callee_pc, first_callee);
  s.sym_by_addr.emplace(second_callee_pc, second_callee);
  return s;
}

sbt::cfg::FunctionVerifyResult verify(const Scenario &s,
                                      const std::string &name) {
  const sbt::cfg::VerifyOptions options{.sym_by_addr = &s.sym_by_addr};
  return sbt::cfg::verify_function(s.cfg, name, options);
}

void check_get_local_id_rejects_barrier() {
  const auto s = make_single_call_branch_cfg(0x1000u, 0x9000u,
                                             "_Z12get_local_idj", true, true);
  const auto result = verify(s, "local_id_barrier");
  require(result.vbranch.size() == 1, "expected one vbranch");
  require(!result.vbranch[0].proven_uniform,
          "get_local_id must make v0 non-uniform");
  require(result.barriers.size() == 1, "expected one barrier");
  require(!result.barriers[0].ok,
          "barrier in get_local_id-derived branch must be rejected");
}

void check_get_group_id_preserves_uniform_dim() {
  const auto s = make_single_call_branch_cfg(0x2000u, 0x9004u,
                                             "_Z12get_group_idj", false, true);
  const auto result = verify(s, "group_id_uniform_dim");
  require(result.vbranch.size() == 1, "expected one vbranch");
  require(result.vbranch[0].proven_uniform,
          "get_group_id with uniform dim must preserve v0 uniformity");
}

void check_get_group_id_nonuniform_dim() {
  const auto s = make_two_call_branch_cfg(0x3000u, 0x9010u,
                                          "_Z12get_local_idj", 0x9014u,
                                          "_Z12get_group_idj");
  const auto result = verify(s, "group_id_nonuniform_dim");
  require(result.vbranch.size() == 1, "expected one vbranch");
  require(!result.vbranch[0].proven_uniform,
          "get_group_id with non-uniform dim must not prove v0 uniform");
}

void check_pure_math_transfer() {
  const auto uniform = make_single_call_branch_cfg(0x4000u, 0x9020u,
                                                   "_Z4sqrtf", false, true);
  const auto uniform_result = verify(uniform, "sqrt_uniform");
  require(uniform_result.vbranch[0].proven_uniform,
          "sqrtf must preserve uniform v0");

  const auto varying = make_two_call_branch_cfg(0x5000u, 0x9024u,
                                                "_Z12get_local_idj", 0x9028u,
                                                "_Z4sqrtf");
  const auto varying_result = verify(varying, "sqrt_nonuniform");
  require(!varying_result.vbranch[0].proven_uniform,
          "sqrtf must not make non-uniform v0 uniform");
}

void check_no_summary_call_boundaries() {
  const auto helper = make_single_call_branch_cfg(0x6000u, 0x9030u,
                                                  "ordinary_helper", false,
                                                  true);
  const auto helper_result = verify(helper, "ordinary_helper");
  require(!helper_result.vbranch[0].proven_uniform,
          "ordinary resolved helper calls must clear vector-uniform facts");

  const auto missing_symbol = make_single_call_branch_cfg(
      0x7000u, 0x9034u, "_Z12get_group_idj", false, false);
  const sbt::cfg::VerifyOptions no_options{};
  const auto missing_result =
      sbt::cfg::verify_function(missing_symbol.cfg, "missing_symbol",
                                no_options);
  require(!missing_result.vbranch[0].proven_uniform,
          "missing symbol map must clear vector-uniform facts");
}

void check_builtin_metadata_contract() {
  std::unordered_set<sbt::BuiltinKind> accepted_kinds;
  for (const auto &entry : sbt::builtin_call_entries()) {
    require(sbt::lookup_builtin_call(entry.name) == entry.kind,
            "builtin lookup entry must resolve to its kind");
    require(sbt::ptx::is_inlined_builtin_call_name(entry.name),
            "public PTX builtin classifier must accept shared entries");
    require(sbt::builtin_summary_for(entry.kind).has_value(),
            "accepted builtin kind must have verifier summary");
    accepted_kinds.insert(entry.kind);
  }

  for (const auto &summary : sbt::builtin_summaries()) {
    require(accepted_kinds.contains(summary.kind),
            "summary kind must be accepted by builtin lookup");
    require(!summary.vector_writes.empty(),
            "summary must describe at least one vector write");
  }

  require(!sbt::ptx::is_inlined_builtin_call_name("__not_a_builtin"),
          "public PTX classifier must reject unknown helpers");
}

} // namespace

int main() {
  check_get_local_id_rejects_barrier();
  check_get_group_id_preserves_uniform_dim();
  check_get_group_id_nonuniform_dim();
  check_pure_math_transfer();
  check_no_summary_call_boundaries();
  check_builtin_metadata_contract();
  return 0;
}
