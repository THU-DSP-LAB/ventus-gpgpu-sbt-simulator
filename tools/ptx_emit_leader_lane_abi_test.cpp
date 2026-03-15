#include "sbt/ptx_emit.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

/*
背景
- `sbt/ptx_emit.cpp` 正在从 shared `WarpCtx` + `.local vctx` 迁移到 leader-lane scalar state + value ABI。

需求/作用
- 固化新主线最关键的可观察 PTX 合同，避免 helper ABI、scalar branch broadcast、structured divergence 同步点回退。

用法
- 构建后直接运行：`./build/ptx_emit_leader_lane_abi_test`

实现原理/处理步骤
- 手工构造小型 `FunctionCfg`，调用 `sbt::ptx::emit_module()` 生成 PTX。
- 对输出做字符串断言，检查：
- helper prototype / definition 是否切到新的 mutable/machine/runtime blob ABI；
- scalar `beq` 是否在 `bra.uni` 前使用 leader broadcast；
- `vbranch/join` 路径是否出现 full-`x` 广播与 join 前驱 shim。
*/

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
}

sbt::cfg::BundleInst make_inst(uint32_t pc, std::string name) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = std::move(name);
  return bi;
}

sbt::cfg::BundleInst make_endprg(uint32_t pc) { return make_inst(pc, "endprg"); }

sbt::cfg::BundleInst make_scalar_branch(uint32_t pc, const std::string &name, int rs1, int rs2, uint32_t target) {
  auto bi = make_inst(pc, name);
  bi.inst.rs1_class = sbt::RegClass::X;
  bi.inst.rs1 = rs1;
  bi.inst.rs2_class = sbt::RegClass::X;
  bi.inst.rs2 = rs2;
  bi.inst.imm_kind = sbt::ImmKind::B13;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  return bi;
}

sbt::cfg::BundleInst make_vbranch(uint32_t pc, const std::string &name, int rs1, int rs2, uint32_t target) {
  auto bi = make_inst(pc, name);
  bi.inst.rs1_class = sbt::RegClass::V;
  bi.inst.rs1 = rs1;
  bi.inst.rs2_class = sbt::RegClass::V;
  bi.inst.rs2 = rs2;
  bi.inst.imm_kind = sbt::ImmKind::B13;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  return bi;
}

sbt::cfg::BundleInst make_regext_vbranch(uint32_t bundle_pc, uint32_t inst_pc, const std::string &name, int rs1, int rs2, uint32_t target) {
  auto bi = make_vbranch(inst_pc, name, rs1, rs2, target);
  bi.pc = bundle_pc;
  bi.inst_pc = inst_pc;
  bi.len = 8;
  return bi;
}

sbt::cfg::BundleInst make_setrpc(uint32_t pc, int rs1, uint32_t join_pc) {
  auto auipc_pc = pc - 4u;
  const int64_t delta = static_cast<int64_t>(join_pc) - static_cast<int64_t>(auipc_pc);
  auto bi = make_inst(pc, "setrpc");
  bi.inst.rs1_class = sbt::RegClass::X;
  bi.inst.rs1 = rs1;
  bi.inst.imm_kind = sbt::ImmKind::I12;
  bi.inst.imm = static_cast<int32_t>(delta);
  return bi;
}

sbt::cfg::BundleInst make_auipc(uint32_t pc, int rd) {
  auto bi = make_inst(pc, "auipc");
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = rd;
  bi.inst.imm_kind = sbt::ImmKind::U20;
  bi.inst.imm = 0;
  return bi;
}

sbt::cfg::BundleInst make_jump(uint32_t pc, uint32_t target) {
  auto bi = make_inst(pc, "jal");
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = 0;
  bi.inst.imm_kind = sbt::ImmKind::J21;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  return bi;
}

sbt::cfg::BundleInst make_call(uint32_t pc, uint32_t target) {
  auto bi = make_inst(pc, "jal");
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = 1;
  bi.inst.imm_kind = sbt::ImmKind::J21;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  return bi;
}

void append_block(sbt::cfg::FunctionCfg &cfg, uint32_t start, std::initializer_list<size_t> inst_indices,
                  std::initializer_list<sbt::cfg::Edge> succs) {
  sbt::cfg::BasicBlock bb;
  bb.start = start;
  bb.inst_indices.assign(inst_indices.begin(), inst_indices.end());
  bb.succs.assign(succs.begin(), succs.end());
  cfg.block_index_by_start.emplace(start, cfg.blocks.size());
  cfg.blocks.push_back(std::move(bb));
}

sbt::cfg::FunctionCfg make_scalar_branch_cfg(uint32_t start) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 12u;
  cfg.insts = {
      make_scalar_branch(start, "beq", 5, 6, start + 8u),
      make_endprg(start + 4u),
      make_endprg(start + 8u),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(start, start);
  cfg.inst_pc_to_block.emplace(start + 4u, start + 4u);
  cfg.inst_pc_to_block.emplace(start + 8u, start + 8u);
  append_block(cfg, start, {0}, {
      sbt::cfg::Edge{start, start + 8u, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{start, start + 4u, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, start + 4u, {1}, {});
  append_block(cfg, start + 8u, {2}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_helper_call_cfg(uint32_t start, uint32_t target) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 8u;
  cfg.insts = {make_call(start, target), make_endprg(start + 4u)};
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
    cfg.inst_pc_to_block.emplace(pc, start);
  }
  append_block(cfg, start, {0, 1}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_regext_scalar_branch_cfg(uint32_t start) {
  const uint32_t branch_bundle_pc = start;
  const uint32_t branch_inst_pc = start + 4u;
  const uint32_t fallthrough_pc = start + 8u;
  const uint32_t target_pc = start + 12u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 16u;
  cfg.insts = {
      make_scalar_branch(branch_inst_pc, "beq", 5, 6, target_pc),
      make_endprg(fallthrough_pc),
      make_endprg(target_pc),
  };
  cfg.insts[0].pc = branch_bundle_pc;
  cfg.insts[0].inst_pc = branch_inst_pc;
  cfg.insts[0].len = 8;

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(branch_bundle_pc, branch_bundle_pc);
  cfg.inst_pc_to_block.emplace(fallthrough_pc, fallthrough_pc);
  cfg.inst_pc_to_block.emplace(target_pc, target_pc);

  append_block(cfg, branch_bundle_pc, {0}, {
      sbt::cfg::Edge{branch_bundle_pc, target_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{branch_bundle_pc, fallthrough_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, fallthrough_pc, {1}, {});
  append_block(cfg, target_pc, {2}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_vbranch_join_cfg(uint32_t start) {
  const uint32_t then_pc = start + 16u;
  const uint32_t else_pc = start + 8u;
  const uint32_t join_pc = start + 24u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 28u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, join_pc),
      make_vbranch(start + 8u, "vbeq", 1, 2, then_pc),
      make_jump(start + 12u, join_pc),
      make_jump(then_pc, join_pc),
      make_inst(join_pc, "join"),
      make_endprg(join_pc + 4u),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(start, start);
  cfg.inst_pc_to_block.emplace(start + 4u, start);
  cfg.inst_pc_to_block.emplace(start + 8u, else_pc);
  cfg.inst_pc_to_block.emplace(start + 12u, else_pc);
  cfg.inst_pc_to_block.emplace(then_pc, then_pc);
  cfg.inst_pc_to_block.emplace(join_pc, join_pc);
  cfg.inst_pc_to_block.emplace(join_pc + 4u, join_pc);

  append_block(cfg, start, {0, 1}, {
      sbt::cfg::Edge{start, else_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, else_pc, {2, 3}, {
      sbt::cfg::Edge{else_pc, then_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{else_pc, join_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, then_pc, {4}, {
      sbt::cfg::Edge{then_pc, join_pc, sbt::cfg::EdgeKind::Jump},
  });
  append_block(cfg, join_pc, {5, 6}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_vbranch_direct_join_cfg(uint32_t start) {
  const uint32_t branch_pc = start + 8u;
  const uint32_t then_pc = start + 12u;
  const uint32_t join_pc = start + 20u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 24u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, join_pc),
      make_vbranch(branch_pc, "vbeq", 1, 2, join_pc),
      make_jump(then_pc, join_pc),
      make_inst(join_pc, "join"),
      make_endprg(join_pc + 4u),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(start, start);
  cfg.inst_pc_to_block.emplace(start + 4u, start);
  cfg.inst_pc_to_block.emplace(branch_pc, branch_pc);
  cfg.inst_pc_to_block.emplace(then_pc, then_pc);
  cfg.inst_pc_to_block.emplace(join_pc, join_pc);
  cfg.inst_pc_to_block.emplace(join_pc + 4u, join_pc);

  append_block(cfg, start, {0, 1}, {
      sbt::cfg::Edge{start, branch_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, branch_pc, {2}, {
      sbt::cfg::Edge{branch_pc, join_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{branch_pc, then_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, then_pc, {3}, {
      sbt::cfg::Edge{then_pc, join_pc, sbt::cfg::EdgeKind::Jump},
  });
  append_block(cfg, join_pc, {4, 5}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_regext_vbranch_direct_join_cfg(uint32_t start) {
  const uint32_t branch_bundle_pc = start + 8u;
  const uint32_t branch_inst_pc = start + 12u;
  const uint32_t then_pc = start + 16u;
  const uint32_t join_pc = start + 20u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 28u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, join_pc),
      make_regext_vbranch(branch_bundle_pc, branch_inst_pc, "vbeq", 1, 2, join_pc),
      make_jump(then_pc, join_pc),
      make_inst(join_pc, "join"),
      make_endprg(join_pc + 4u),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(start, start);
  cfg.inst_pc_to_block.emplace(start + 4u, start);
  cfg.inst_pc_to_block.emplace(branch_bundle_pc, branch_bundle_pc);
  cfg.inst_pc_to_block.emplace(then_pc, then_pc);
  cfg.inst_pc_to_block.emplace(join_pc, join_pc);
  cfg.inst_pc_to_block.emplace(join_pc + 4u, join_pc);

  append_block(cfg, start, {0, 1}, {
      sbt::cfg::Edge{start, branch_bundle_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, branch_bundle_pc, {2}, {
      sbt::cfg::Edge{branch_bundle_pc, join_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{branch_bundle_pc, then_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, then_pc, {3}, {
      sbt::cfg::Edge{then_pc, join_pc, sbt::cfg::EdgeKind::Jump},
  });
  append_block(cfg, join_pc, {4, 5}, {});
  return cfg;
}

size_t count_substr(const std::string &text, const std::string &needle) {
  if (needle.empty()) return 0;
  size_t count = 0;
  size_t pos = 0;
  while (true) {
    pos = text.find(needle, pos);
    if (pos == std::string::npos) break;
    ++count;
    pos += needle.size();
  }
  return count;
}

} // namespace

int main() {
  constexpr uint32_t kEntryPc = 0x80001000u;
  constexpr uint32_t kHelperAPc = 0x80001100u;
  constexpr uint32_t kHelperBPc = 0x80001200u;
  constexpr uint32_t kBranchPc = 0x80001300u;
  constexpr uint32_t kDivergePc = 0x80001400u;
  constexpr uint32_t kDirectJoinPc = 0x80001500u;
  constexpr uint32_t kRegextDirectJoinPc = 0x80001600u;
  constexpr uint32_t kRegextScalarBranchPc = 0x80001700u;

  const auto entry_cfg = make_helper_call_cfg(kEntryPc, kHelperAPc);
  const auto helper_a_cfg = make_helper_call_cfg(kHelperAPc, kHelperBPc);
  const auto helper_b_cfg = make_scalar_branch_cfg(kBranchPc);
  const auto diverge_cfg = make_vbranch_join_cfg(kDivergePc);
  const auto direct_join_cfg = make_vbranch_direct_join_cfg(kDirectJoinPc);
  const auto regext_direct_join_cfg = make_regext_vbranch_direct_join_cfg(kRegextDirectJoinPc);
  const auto regext_scalar_branch_cfg = make_regext_scalar_branch_cfg(kRegextScalarBranchPc);

  std::vector<sbt::ptx::FuncToEmit> funcs;
  funcs.push_back(sbt::ptx::FuncToEmit{"helper_a", "__sbt_fn_A", helper_a_cfg});
  funcs.push_back(sbt::ptx::FuncToEmit{"helper_b", "__sbt_fn_B", helper_b_cfg});
  funcs.push_back(sbt::ptx::FuncToEmit{"diverge", "__sbt_fn_diverge", diverge_cfg});

  std::unordered_map<uint32_t, std::string> sym_by_addr;
  sym_by_addr.emplace(kEntryPc, "kernel");
  sym_by_addr.emplace(kHelperAPc, "helper_a");
  sym_by_addr.emplace(kHelperBPc, "helper_b");
  sym_by_addr.emplace(kBranchPc, "branch_helper");
  sym_by_addr.emplace(kDivergePc, "diverge");

  std::unordered_map<uint32_t, std::string> ptx_name_by_addr;
  ptx_name_by_addr.emplace(kHelperAPc, "__sbt_fn_A");
  ptx_name_by_addr.emplace(kHelperBPc, "__sbt_fn_B");
  ptx_name_by_addr.emplace(kDivergePc, "__sbt_fn_diverge");

  sbt::ptx::Options opt;
  opt.include_comments = false;

  const auto res = sbt::ptx::emit_module(entry_cfg, sym_by_addr, "kernel", funcs, ptx_name_by_addr, opt);
  const std::string &ptx = res.ptx;
  const auto direct_join_res = sbt::ptx::emit_module(direct_join_cfg, {{kDirectJoinPc, "direct_join"}}, "direct_join", {}, {}, opt);
  const std::string &direct_join_ptx = direct_join_res.ptx;
  const auto regext_direct_join_res =
      sbt::ptx::emit_module(regext_direct_join_cfg, {{kRegextDirectJoinPc, "regext_direct_join"}}, "regext_direct_join", {}, {}, opt);
  const std::string &regext_direct_join_ptx = regext_direct_join_res.ptx;
  const auto regext_scalar_branch_res =
      sbt::ptx::emit_module(regext_scalar_branch_cfg, {{kRegextScalarBranchPc, "regext_scalar_branch"}}, "regext_scalar_branch", {}, {}, opt);
  const std::string &regext_scalar_branch_ptx = regext_scalar_branch_res.ptx;

  require(ptx.find(".param .align 4 .b8 __sbt_mutable_state_in[") != std::string::npos,
          "helper definition should use mutable-state input blob");
  require(ptx.find(".param .align 4 .b8 __sbt_machine_ctx_in[") != std::string::npos,
          "helper definition should use machine-context blob");
  require(ptx.find(".param .align 8 .b8 __sbt_runtime_env_in[") != std::string::npos,
          "helper definition should use runtime-env blob");
  require(ptx.find(".param .align 4 .b8 __sbt_arg_vctx_base") == std::string::npos,
          "legacy vctx ABI must be removed from helper signature");
  require(ptx.find("call.uni (__sbt_call_mutable_out") != std::string::npos,
          "direct call should return mutable-state blob explicitly");

  require(ptx.find("shfl.sync.idx.b32 %r14") != std::string::npos,
          "scalar branch should broadcast leader scalar operands before bra.uni");
  require(ptx.find("bra.uni BB_80001308;") != std::string::npos,
          "scalar branch should remain bra.uni-based");

  require(ptx.find("shfl.sync.idx.b32 %x1, %x1, %r2, 0x1f, %r1;") != std::string::npos,
          "structured divergence should broadcast full x-state from current leader");
  require(ptx.find("_sbt_join_edge_") != std::string::npos,
          "join predecessor edges should use explicit shim labels");
  require(direct_join_ptx.find("bfind.u32 %r2, %r1;\n  setp.eq.u32 %p0, %r0, %r2;\n  bra BB_80001514;") == std::string::npos,
          "vbranch direct-to-join path must not skip join-edge reconverge via path-entry leader reset");
  require(count_substr(direct_join_ptx, "_sbt_join_edge_") >= 4,
          "vbranch direct-to-join shape should create reconverge shims for both join predecessors");
  require(regext_direct_join_ptx.find("_sbt_join_edge_") != std::string::npos,
          "regext-bundled vbranch should still resolve join-edge shims");
  require(regext_direct_join_ptx.find("bra _sbt_join_edge_") != std::string::npos,
          "regext-bundled vbranch should branch via synthesized edge labels");
  require(regext_scalar_branch_ptx.find("bra.uni BB_8000170c;") != std::string::npos,
          "regext-bundled scalar branch should still resolve branch target block labels");
  require(regext_scalar_branch_ptx.find("bra.uni BB_80001708;") != std::string::npos,
          "regext-bundled scalar branch should still resolve fallthrough block labels");
  require(count_substr(ptx, "bfind.u32 %r2, %r1;") < 6,
          "new emitter should not materialize legacy leader preamble on every block");

  std::cout << "ok ptx leader-lane abi\n";
  return 0;
}
