/*
背景
- `work_group_broadcast` 是 OpenCL work-group collective，不能通过普通 direct-call helper 路径落到 libclc scratch/global addressing。
- 当前 PTX emitter 对 32-bit float/int/uint overload 做 inline lowering，并用 per-CTA shared slot 与 `bar.sync` 实现广播。

需求/作用
- 回归验证 supported `work_group_broadcast` overload 会被内联为 PTX shared-memory broadcast。
- 验证 unsupported overload 显式 fail-fast，避免静默走普通 helper call 或错误地址映射。

用法
- 构建后运行：`timeout 60s build/workgroup_broadcast_builtin_test`

实现原理/处理步骤
- 手工构造一个 direct `jal` 到 builtin symbol 的最小 `FunctionCfg`。
- 调用 `sbt::ptx::emit_kernel()` 生成 PTX 文本。
- 检查 shared slot 声明、source lane predicate、store/barrier/load/barrier 顺序，以及 unsupported overload 的诊断。
*/

#include "sbt/ptx_emit.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
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
  if (bi.inst.inst_id == sbt::kUnknownInstId) {
    bi.inst.inst_id = sbt::make_inst_id(name);
  }
  return bi;
}

sbt::cfg::BundleInst make_call(uint32_t pc, uint32_t target) {
  auto bi = make_inst(pc, "jal");
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = 1;
  bi.inst.imm_kind = sbt::ImmKind::J21;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::FunctionCfg make_cfg(uint32_t start, std::vector<sbt::cfg::BundleInst> insts) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = insts.back().pc + 4u;
  cfg.insts = std::move(insts);

  sbt::cfg::BasicBlock bb;
  bb.start = start;
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const uint32_t pc = cfg.insts[i].pc;
    bb.inst_indices.push_back(i);
    cfg.inst_index_by_pc.emplace(pc, i);
    cfg.inst_pc_to_block.emplace(pc, start);
  }
  cfg.block_index_by_start.emplace(start, 0);
  cfg.blocks.push_back(std::move(bb));
  return cfg;
}

size_t count_substr(const std::string &text, const std::string &needle) {
  size_t count = 0;
  for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

size_t require_after(const std::string &text, const std::string &needle, size_t start, const std::string &msg) {
  const size_t pos = text.find(needle, start);
  require(pos != std::string::npos, msg);
  return pos + needle.size();
}

void check_broadcast(const std::string &callee, bool expect_y, bool expect_z) {
  constexpr uint32_t kStart = 0x80001000u;
  constexpr uint32_t kTarget = 0x80002000u;
  auto endprg = make_inst(kStart + 4u, "endprg");
  const auto cfg = make_cfg(kStart, {make_call(kStart, kTarget), endprg});

  sbt::ptx::Options opt;
  opt.include_comments = false;
  const auto ptx = sbt::ptx::emit_kernel(cfg, {{kStart, "kernel"}, {kTarget, callee}}, "kernel", opt).ptx;

  require(ptx.find(".shared .align 4 .u32 __sbt_workgroup_broadcast_slot;") != std::string::npos,
          "broadcast lowering should declare one per-CTA shared slot");
  size_t pos = require_after(ptx, "st.shared.u32 [__sbt_workgroup_broadcast_slot], %v0;", 0,
                             "source work-item should write v0 into the shared slot");
  pos = require_after(ptx, "bar.sync 0;", pos,
                      "broadcast lowering should synchronize after the source write");
  pos = require_after(ptx, "ld.shared.u32 %v0, [__sbt_workgroup_broadcast_slot];", pos,
                      "all work-items should load the broadcast value from the shared slot");
  (void)require_after(ptx, "bar.sync 0;", pos,
                      "broadcast lowering should synchronize after the shared-slot read");
  require(count_substr(ptx, "__sbt_workgroup_broadcast_slot") == 3,
          "broadcast slot should appear only in declaration/store/load");
  require(ptx.find("mov.u32") != std::string::npos && ptx.find("%tid.x;") != std::string::npos &&
              ptx.find(", %v1;") != std::string::npos,
          "broadcast source predicate should compare x local id");
  require((ptx.find("%tid.y;") != std::string::npos && ptx.find(", %v2;") != std::string::npos) == expect_y,
          "broadcast source predicate y dimension mismatch");
  require((ptx.find("%tid.z;") != std::string::npos && ptx.find(", %v3;") != std::string::npos) == expect_z,
          "broadcast source predicate z dimension mismatch");
  require(ptx.find("__sbt_workgroup_scratch") == std::string::npos,
          "broadcast lowering must not private-map libclc scratch globals");
  require(ptx.find("%nctaid.x") == std::string::npos,
          "broadcast lowering must not derive an address interval from grid size");
  require(ptx.find("call.uni") == std::string::npos,
          "work_group_broadcast builtin should be inlined rather than emitted as a helper call");
}

void check_unsupported_broadcast_overload_fails_fast() {
  constexpr uint32_t kStart = 0x80001000u;
  constexpr uint32_t kTarget = 0x80002000u;
  auto endprg = make_inst(kStart + 4u, "endprg");
  const auto cfg = make_cfg(kStart, {make_call(kStart, kTarget), endprg});

  sbt::ptx::Options opt;
  opt.include_comments = false;
  try {
    (void)sbt::ptx::emit_kernel(cfg, {{kStart, "kernel"}, {kTarget, "_Z20work_group_broadcastlj"}}, "kernel", opt);
  } catch (const sbt::ptx::EmitError &e) {
    require(e.code == "unsupported.work_group_broadcast",
            "unsupported work_group_broadcast overload should have a specific error code");
    require(std::string_view(e.what()).find("_Z20work_group_broadcastlj") != std::string_view::npos,
            "unsupported work_group_broadcast diagnostic should include callee");
    return;
  }
  throw std::runtime_error("assert: unsupported work_group_broadcast overload should fail fast");
}

} // namespace

int main() {
  check_broadcast("_Z20work_group_broadcastfj", false, false);
  check_broadcast("_Z20work_group_broadcastfjj", true, false);
  check_broadcast("_Z20work_group_broadcastfjjj", true, true);
  check_broadcast("_Z20work_group_broadcastij", false, false);
  check_broadcast("_Z20work_group_broadcastijj", true, false);
  check_broadcast("_Z20work_group_broadcastijjj", true, true);
  check_broadcast("_Z20work_group_broadcastjj", false, false);
  check_broadcast("_Z20work_group_broadcastjjj", true, false);
  check_broadcast("_Z20work_group_broadcastjjjj", true, true);
  check_unsupported_broadcast_overload_fails_fast();
  std::cout << "ok workgroup broadcast builtin\n";
  return 0;
}
