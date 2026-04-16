#include "sbt/ptx_emit.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

/*
背景
- `sbt/ptx_emit.cpp` 正在从 leader-owned scalar state 迁移到 replicated active-lane scalar state。

需求/作用
- 固化 replicated scalar-state 主线最关键的可观察 PTX 合同，避免 helper ABI、fixed-lane scalarization、lazy leader 选择与 divergence 协议回退。

用法
- 构建后直接运行：`./build/ptx_emit_leader_lane_abi_test`

实现原理/处理步骤
- 手工构造小型 `FunctionCfg`，调用 `sbt::ptx::emit_module()` 生成 PTX。
- 对输出做字符串断言，检查：
- helper prototype / definition 是否使用 replicated scalar-state 下的 mutable/machine/runtime blob ABI；
- scalar `beq` 是否直接读取 replicated `xreg`，而不是做 leader broadcast；
- `vbranch/join` 是否移除 full-`x` 广播与 join 前驱 shim；
- `vmv.x.s` 是否保留固定 lane 语义并显式复制回 replicated `xreg`；
- 分歧路径中的 leader-only store 是否按需懒选择 leader，而不是在路径入口统一切换。
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
  (void)sbt::populate_inst_metadata(bi.inst.name, bi.inst);
  if (bi.inst.inst_id == sbt::kUnknownInstId && bi.inst.name != "unknown") bi.inst.inst_id = sbt::make_inst_id(bi.inst.name);
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

sbt::cfg::BundleInst make_addi(uint32_t pc, int rd, int rs1, int imm) {
  auto bi = make_inst(pc, "addi");
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = rd;
  bi.inst.rs1_class = sbt::RegClass::X;
  bi.inst.rs1 = rs1;
  bi.inst.imm_kind = sbt::ImmKind::I12;
  bi.inst.imm = imm;
  return bi;
}

sbt::cfg::BundleInst make_scalar_store(uint32_t pc, const std::string &name, int rs1, int rs2, int imm) {
  auto bi = make_inst(pc, name);
  bi.inst.rs1_class = sbt::RegClass::X;
  bi.inst.rs1 = rs1;
  bi.inst.rs2_class = sbt::RegClass::X;
  bi.inst.rs2 = rs2;
  bi.inst.imm_kind = sbt::ImmKind::S12;
  bi.inst.imm = imm;
  return bi;
}

sbt::cfg::BundleInst make_vmv_x_s(uint32_t pc, int rd, int rs2) {
  auto bi = make_inst(pc, "vmv_x_s");
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = rd;
  bi.inst.rs2_class = sbt::RegClass::V;
  bi.inst.rs2 = rs2;
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

sbt::cfg::FunctionCfg make_vmv_x_s_cfg(uint32_t start) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 12u;
  cfg.insts = {
      make_vmv_x_s(start, 5, 3),
      make_addi(start + 4u, 6, 5, 1),
      make_endprg(start + 8u),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
    cfg.inst_pc_to_block.emplace(pc, start);
  }
  append_block(cfg, start, {0, 1, 2}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_vbranch_store_cfg(uint32_t start) {
  const uint32_t branch_pc = start + 8u;
  const uint32_t then_pc = start + 20u;
  const uint32_t join_pc = start + 24u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 28u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, join_pc),
      make_vbranch(branch_pc, "vbeq", 1, 2, then_pc),
      make_scalar_store(start + 12u, "sw", 10, 11, 0),
      make_jump(start + 20u, join_pc),
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
  cfg.inst_pc_to_block.emplace(start + 12u, branch_pc);
  cfg.inst_pc_to_block.emplace(then_pc, then_pc);
  cfg.inst_pc_to_block.emplace(join_pc, join_pc);
  cfg.inst_pc_to_block.emplace(join_pc + 4u, join_pc);

  append_block(cfg, start, {0, 1}, {
      sbt::cfg::Edge{start, branch_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, branch_pc, {2, 3}, {
      sbt::cfg::Edge{branch_pc, then_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{branch_pc, join_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, then_pc, {4}, {
      sbt::cfg::Edge{then_pc, join_pc, sbt::cfg::EdgeKind::Jump},
  });
  append_block(cfg, join_pc, {5, 6}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_vbranch_store_call_cfg(uint32_t start, uint32_t target) {
  const uint32_t branch_pc = start + 8u;
  const uint32_t then_pc = start + 20u;
  const uint32_t join_pc = start + 24u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 32u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, join_pc),
      make_vbranch(branch_pc, "vbeq", 1, 2, then_pc),
      make_scalar_store(start + 12u, "sw", 10, 11, 0),
      make_jump(start + 20u, join_pc),
      make_call(join_pc, target),
      make_endprg(join_pc + 4u),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(start, start);
  cfg.inst_pc_to_block.emplace(start + 4u, start);
  cfg.inst_pc_to_block.emplace(branch_pc, branch_pc);
  cfg.inst_pc_to_block.emplace(start + 12u, branch_pc);
  cfg.inst_pc_to_block.emplace(then_pc, then_pc);
  cfg.inst_pc_to_block.emplace(join_pc, join_pc);
  cfg.inst_pc_to_block.emplace(join_pc + 4u, join_pc);

  append_block(cfg, start, {0, 1}, {
      sbt::cfg::Edge{start, branch_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, branch_pc, {2, 3}, {
      sbt::cfg::Edge{branch_pc, then_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{branch_pc, join_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, then_pc, {4}, {
      sbt::cfg::Edge{then_pc, join_pc, sbt::cfg::EdgeKind::Jump},
  });
  append_block(cfg, join_pc, {5, 6}, {});
  return cfg;
}

sbt::cfg::FunctionCfg make_helper_vbranch_store_ret_cfg(uint32_t start) {
  const uint32_t branch_pc = start + 8u;
  const uint32_t then_pc = start + 20u;
  const uint32_t join_pc = start + 24u;

  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 32u;
  cfg.insts = {
      make_auipc(start, 1),
      make_setrpc(start + 4u, 1, join_pc),
      make_vbranch(branch_pc, "vbeq", 1, 2, then_pc),
      make_scalar_store(start + 12u, "sw", 10, 11, 0),
      make_jump(start + 20u, join_pc),
      make_endprg(join_pc),
  };
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    cfg.inst_index_by_pc.emplace(pc, i);
  }
  cfg.inst_pc_to_block.emplace(start, start);
  cfg.inst_pc_to_block.emplace(start + 4u, start);
  cfg.inst_pc_to_block.emplace(branch_pc, branch_pc);
  cfg.inst_pc_to_block.emplace(start + 12u, branch_pc);
  cfg.inst_pc_to_block.emplace(then_pc, then_pc);
  cfg.inst_pc_to_block.emplace(join_pc, join_pc);

  append_block(cfg, start, {0, 1}, {
      sbt::cfg::Edge{start, branch_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, branch_pc, {2, 3}, {
      sbt::cfg::Edge{branch_pc, then_pc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{branch_pc, join_pc, sbt::cfg::EdgeKind::Fallthrough},
  });
  append_block(cfg, then_pc, {4}, {
      sbt::cfg::Edge{then_pc, join_pc, sbt::cfg::EdgeKind::Jump},
  });
  append_block(cfg, join_pc, {5}, {});
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

std::string extract_ptx_body(const std::string &ptx, const std::string &header) {
  const size_t start = ptx.rfind(header);
  require(start != std::string::npos, "missing PTX header: " + header);
  const size_t body_end = ptx.find("\n}\n\n", start);
  require(body_end != std::string::npos, "missing PTX body end for: " + header);
  return ptx.substr(start, body_end - start);
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
  constexpr uint32_t kVmvXSBasicPc = 0x80001800u;
  constexpr uint32_t kVbranchStorePc = 0x80001900u;
  constexpr uint32_t kVbranchStoreCallPc = 0x80001a00u;
  constexpr uint32_t kJoinCallTargetPc = 0x80001b00u;
  constexpr uint32_t kHelperStoreRetPc = 0x80001c00u;

  const auto entry_cfg = make_helper_call_cfg(kEntryPc, kHelperAPc);
  const auto helper_a_cfg = make_helper_call_cfg(kHelperAPc, kHelperBPc);
  const auto helper_b_cfg = make_scalar_branch_cfg(kBranchPc);
  const auto diverge_cfg = make_vbranch_join_cfg(kDivergePc);
  const auto direct_join_cfg = make_vbranch_direct_join_cfg(kDirectJoinPc);
  const auto regext_direct_join_cfg = make_regext_vbranch_direct_join_cfg(kRegextDirectJoinPc);
  const auto regext_scalar_branch_cfg = make_regext_scalar_branch_cfg(kRegextScalarBranchPc);
  const auto vmv_x_s_cfg = make_vmv_x_s_cfg(kVmvXSBasicPc);
  const auto vbranch_store_cfg = make_vbranch_store_cfg(kVbranchStorePc);
  const auto vbranch_store_call_cfg = make_vbranch_store_call_cfg(kVbranchStoreCallPc, kJoinCallTargetPc);
  const auto join_call_target_cfg = make_helper_call_cfg(kJoinCallTargetPc, kHelperBPc);
  const auto helper_store_ret_cfg = make_helper_vbranch_store_ret_cfg(kHelperStoreRetPc);

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
  const auto vmv_x_s_res = sbt::ptx::emit_module(vmv_x_s_cfg, {{kVmvXSBasicPc, "vmv_x_s"}}, "vmv_x_s", {}, {}, opt);
  const std::string &vmv_x_s_ptx = vmv_x_s_res.ptx;
  const auto vbranch_store_res = sbt::ptx::emit_module(vbranch_store_cfg, {{kVbranchStorePc, "vbranch_store"}}, "vbranch_store", {}, {}, opt);
  const std::string &vbranch_store_ptx = vbranch_store_res.ptx;
  const auto vbranch_store_call_res = sbt::ptx::emit_module(
      vbranch_store_call_cfg,
      {{kVbranchStoreCallPc, "vbranch_store_call"}, {kJoinCallTargetPc, "join_call_target"}, {kHelperBPc, "helper_b"}},
      "vbranch_store_call",
      {sbt::ptx::FuncToEmit{"join_call_target", "__sbt_fn_join_call_target", join_call_target_cfg},
       sbt::ptx::FuncToEmit{"helper_b", "__sbt_fn_B", helper_b_cfg}},
      {{kJoinCallTargetPc, "__sbt_fn_join_call_target"}, {kHelperBPc, "__sbt_fn_B"}},
      opt);
  const std::string &vbranch_store_call_ptx = vbranch_store_call_res.ptx;
  const auto helper_store_ret_res = sbt::ptx::emit_module(
      make_helper_call_cfg(kEntryPc, kHelperStoreRetPc),
      {{kEntryPc, "kernel"}, {kHelperStoreRetPc, "helper_store_ret"}},
      "kernel",
      {sbt::ptx::FuncToEmit{"helper_store_ret", "__sbt_fn_helper_store_ret", helper_store_ret_cfg}},
      {{kHelperStoreRetPc, "__sbt_fn_helper_store_ret"}},
      opt);
  const std::string &helper_store_ret_ptx = helper_store_ret_res.ptx;
  const std::string vbranch_store_call_entry_body =
      extract_ptx_body(vbranch_store_call_ptx, ".visible .entry vbranch_store_call(");
  const std::string helper_store_ret_body =
      extract_ptx_body(helper_store_ret_ptx, ") __sbt_fn_helper_store_ret(");

  require(ptx.find(".param .align 4 .b8 __sbt_mutable_state_in[") != std::string::npos,
          "helper definition should use mutable-state input blob");
  require(ptx.find(".param .align 4 .b8 __sbt_machine_ctx_in[") != std::string::npos,
          "helper definition should use machine-context blob");
  require(ptx.find(".param .align 8 .b8 __sbt_runtime_env_in[") != std::string::npos,
          "helper definition should use runtime-env blob");
  require(ptx.find(".param .align 4 .b8 __sbt_arg_vctx_base") == std::string::npos,
          "legacy vctx ABI must be removed from helper signature");
  require(ptx.find(".param .u64 elf_base") == std::string::npos,
          "entry ABI should not keep legacy elf_base");
  require(ptx.find(".param .u64 heap_base") == std::string::npos,
          "entry ABI should not keep legacy heap_base");
  require(ptx.find(".param .u64 global_base") != std::string::npos,
          "entry ABI should expose one global_base parameter");
  require(ptx.find("call.uni (__sbt_call_mutable_out") != std::string::npos,
          "direct call should return mutable-state blob explicitly");
  require(ptx.find(".reg .b32 %tmp_b32_0;") != std::string::npos,
          "entry/helper PTX should declare virtual temporary registers at function scope");
  require(ptx.find(".reg .b64 %tmp_b64_0;") != std::string::npos,
          "entry/helper PTX should declare virtual 64-bit temporaries at function scope");
  require(ptx.find("%tmp_b32_0") != std::string::npos,
          "entry/helper PTX should use %tmp* temporaries in body emission");

  require(ptx.find("shfl.sync.idx.b32 %r14") == std::string::npos,
          "scalar branch should not broadcast replicated scalar operands before bra.uni");
  require(ptx.find("mov.u32 %r14, %x5;") != std::string::npos,
          "scalar branch should read replicated scalar operands directly from xreg state");
  require(ptx.find("bra.uni BB_80001308;") != std::string::npos,
          "scalar branch should remain bra.uni-based");

  require(ptx.find("shfl.sync.idx.b32 %x1, %x1, %r2, 0x1f, %r1;") == std::string::npos,
          "structured divergence should not broadcast the full x-state payload");
  require(ptx.find("_sbt_join_edge_") == std::string::npos,
          "structured divergence should not synthesize join-edge scalar-state shims");
  require(direct_join_ptx.find("_sbt_join_edge_") == std::string::npos,
          "vbranch direct-to-join path should branch directly to reconverged blocks");
  require(direct_join_ptx.find("bra BB_80001514;") != std::string::npos,
          "vbranch direct-to-join path should still branch to the join block");
  require(regext_direct_join_ptx.find("_sbt_join_edge_") == std::string::npos,
          "regext-bundled vbranch should not need synthesized join-edge labels");
  require(regext_direct_join_ptx.find("bra BB_80001614;") != std::string::npos,
          "regext-bundled vbranch should still resolve direct branch targets");
  require(regext_scalar_branch_ptx.find("bra.uni BB_8000170c;") != std::string::npos,
          "regext-bundled scalar branch should still resolve branch target block labels");
  require(regext_scalar_branch_ptx.find("bra.uni BB_80001708;") != std::string::npos,
          "regext-bundled scalar branch should still resolve fallthrough block labels");
  require(vmv_x_s_ptx.find("shfl.sync.idx.b32 %x5, %x5, 0, 0x1f, %r1;") != std::string::npos,
          "vmv.x.s should re-replicate the extracted scalar value from architectural lane 0");
  require(vmv_x_s_ptx.find("trap;") != std::string::npos,
          "vmv.x.s should reject unsupported runtime shapes when the fixed source lane is inactive");
  require(vmv_x_s_ptx.find("mov.u32 %r14, %x5;") != std::string::npos,
          "later scalar consumers should read the re-replicated vmv.x.s result directly");

  require(vbranch_store_ptx.find("_sbt_path_entry_") == std::string::npos,
          "divergent all-lane paths should not synthesize path-entry leader handoff labels");
  require(vbranch_store_ptx.find("_sbt_join_edge_") == std::string::npos,
          "divergent store paths should not rely on join-edge scalar-state payload labels");
  require(count_substr(vbranch_store_ptx, "bfind.u32 %r2, %r1;") == 2,
          "leader should be selected lazily only for the actual leader-only store");
  require(count_substr(vbranch_store_call_entry_body, "bfind.u32 %r2, %r1;") == 3,
          "reconverged direct call after divergent store should re-unify leader metadata before marshaling mutable state");
  require(count_substr(helper_store_ret_body, "bfind.u32 %r2, %r1;") == 2,
          "helper ret after divergent store should re-unify leader metadata before exporting mutable state");

  {
    auto mystery = make_inst(0x80001d00u, "mystery_scalar");
    mystery.inst.rd_class = sbt::RegClass::X;
    mystery.inst.rd = 1;
    mystery.inst.rs1_class = sbt::RegClass::X;
    mystery.inst.rs1 = 2;
    mystery.inst.rs2_class = sbt::RegClass::X;
    mystery.inst.rs2 = 3;

    sbt::cfg::FunctionCfg cfg;
    cfg.start = mystery.pc;
    cfg.end = mystery.pc + 8u;
    cfg.insts = {mystery, make_endprg(mystery.pc + 4u)};
    cfg.inst_index_by_pc.emplace(cfg.insts[0].pc, 0);
    cfg.inst_index_by_pc.emplace(cfg.insts[1].pc, 1);
    cfg.inst_pc_to_block.emplace(cfg.insts[0].pc, cfg.start);
    cfg.inst_pc_to_block.emplace(cfg.insts[1].pc, cfg.start);
    sbt::cfg::BasicBlock bb;
    bb.start = cfg.start;
    bb.inst_indices = {0, 1};
    cfg.block_index_by_start.emplace(bb.start, 0);
    cfg.blocks = {bb};

    bool threw = false;
    try {
      (void)sbt::ptx::emit_module(cfg, {{cfg.start, "mystery_scalar"}}, "mystery_scalar", {}, {}, opt);
    } catch (const sbt::ptx::EmitError &e) {
      threw = e.code == "missing.scalar_exec_metadata";
    }
    require(threw, "unclassified scalar instructions must fail explicitly before lowering");
  }

  {
    auto bad_branch_cfg = make_scalar_branch_cfg(0x80001e00u);
    bad_branch_cfg.insts[0].inst.scalar_exec_kind = sbt::ScalarExecKind::ExternallySideEffecting;

    bool threw = false;
    try {
      (void)sbt::ptx::emit_module(bad_branch_cfg, {{bad_branch_cfg.start, "bad_branch"}}, "bad_branch", {}, {}, opt);
    } catch (const sbt::ptx::EmitError &e) {
      threw = e.code == "invalid.scalar_exec";
    }
    require(threw, "scalar branch lowering must reject non-uniform-pure classification drift");
  }

  {
    const uint32_t start = 0x80001f00u;
    sbt::cfg::FunctionCfg cfg;
    cfg.start = start;
    cfg.end = start + 8u;
    cfg.insts = {make_addi(start, 1, 2, 4), make_endprg(start + 4u)};
    cfg.insts[0].inst.scalar_exec_kind = sbt::ScalarExecKind::ExternallySideEffecting;
    for (size_t i = 0; i < cfg.insts.size(); ++i) {
      cfg.inst_index_by_pc.emplace(cfg.insts[i].pc, i);
      cfg.inst_pc_to_block.emplace(cfg.insts[i].pc, start);
    }
    append_block(cfg, start, {0, 1}, {});

    bool threw = false;
    try {
      (void)sbt::ptx::emit_module(cfg, {{cfg.start, "bad_addi"}}, "bad_addi", {}, {}, opt);
    } catch (const sbt::ptx::EmitError &e) {
      threw = e.code == "invalid.scalar_exec";
    }
    require(threw, "scalar ALU lowering must reject non-uniform-pure classification drift");
  }

  std::cout << "ok ptx replicated scalar state\n";
  return 0;
}
