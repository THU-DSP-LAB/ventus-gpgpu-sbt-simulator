#include "sbt/ptx_emit.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
}

sbt::cfg::BundleInst make_endprg(uint32_t pc) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = "endprg";
  (void)sbt::populate_inst_metadata(bi.inst.name, bi.inst);
  sbt::finalize_emit_descriptor(bi.inst);
  bi.inst.inst_id = sbt::make_inst_id(bi.inst.name);
  return bi;
}

sbt::cfg::BundleInst make_call(uint32_t pc, uint32_t target) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = "jal";
  (void)sbt::populate_inst_metadata(bi.inst.name, bi.inst);
  bi.inst.rd_class = sbt::RegClass::X;
  bi.inst.rd = 1;
  bi.inst.imm_kind = sbt::ImmKind::J21;
  bi.inst.imm = static_cast<int32_t>(target - pc);
  sbt::finalize_emit_descriptor(bi.inst);
  bi.inst.inst_id = sbt::make_inst_id(bi.inst.name);
  return bi;
}

sbt::cfg::FunctionCfg make_cfg(uint32_t start, std::vector<sbt::cfg::BundleInst> insts) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = insts.empty() ? (start + 4u) : (insts.back().inst_pc + 4u);
  cfg.insts = std::move(insts);

  sbt::cfg::BasicBlock bb;
  bb.start = start;
  bb.inst_indices.reserve(cfg.insts.size());
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    bb.inst_indices.push_back(i);
    cfg.inst_index_by_pc.emplace(pc, i);
    cfg.inst_pc_to_block.emplace(pc, start);
  }
  cfg.blocks.push_back(std::move(bb));
  cfg.block_index_by_start.emplace(start, 0);
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

  const auto entry_cfg = make_cfg(kEntryPc, {make_endprg(kEntryPc)});
  const auto helper_a_cfg = make_cfg(kHelperAPc, {make_call(kHelperAPc, kHelperBPc), make_endprg(kHelperAPc + 4u)});
  const auto helper_b_cfg = make_cfg(kHelperBPc, {make_endprg(kHelperBPc)});

  std::vector<sbt::ptx::FuncToEmit> funcs;
  funcs.push_back(sbt::ptx::FuncToEmit{"helper_a", "__sbt_fn_A", helper_a_cfg});
  funcs.push_back(sbt::ptx::FuncToEmit{"helper_b", "__sbt_fn_B", helper_b_cfg});

  std::unordered_map<uint32_t, std::string> sym_by_addr;
  sym_by_addr.emplace(kEntryPc, "kernel");
  sym_by_addr.emplace(kHelperAPc, "helper_a");
  sym_by_addr.emplace(kHelperBPc, "helper_b");

  std::unordered_map<uint32_t, std::string> ptx_name_by_addr;
  ptx_name_by_addr.emplace(kHelperAPc, "__sbt_fn_A");
  ptx_name_by_addr.emplace(kHelperBPc, "__sbt_fn_B");

  sbt::ptx::Options opt;
  opt.include_comments = false;

  const auto res = sbt::ptx::emit_module(entry_cfg, sym_by_addr, "kernel", funcs, ptx_name_by_addr, opt);
  const std::string &ptx = res.ptx;

  const size_t call_pos = ptx.find(", __sbt_fn_B, (");
  require(call_pos != std::string::npos, "helper_a should call helper_b");

  const size_t first_b_func = ptx.find(") __sbt_fn_B(");
  require(first_b_func != std::string::npos, "missing helper_b declaration/definition");
  require(first_b_func < call_pos, "helper_b prototype must appear before forward call");

  require(count_substr(ptx, ") __sbt_fn_B(") == 2, "helper_b should have prototype + definition");
  require(ptx.find("    .param .align 4 .b8 __sbt_mutable_state_in[") != std::string::npos,
          "prototype signature should include mutable-state input blob");
  require(ptx.find("    .param .align 4 .b8 __sbt_machine_ctx_in[") != std::string::npos,
          "prototype signature should include machine-context blob");
  require(ptx.find("    .param .align 8 .b8 __sbt_runtime_env_in[") != std::string::npos,
          "prototype signature should include runtime-env blob");
  require(ptx.find("    .param .u64 __sbt_arg_vctx_base") == std::string::npos,
          "legacy vctx parameter should be removed");
  require(ptx.find(".param .u64 elf_base") == std::string::npos,
          "kernel ABI should not keep legacy elf_base");
  require(ptx.find(".param .u64 heap_base") == std::string::npos,
          "kernel ABI should not keep legacy heap_base");
  require(ptx.find(".param .u64 global_base") != std::string::npos,
          "kernel ABI should expose one global_base parameter");
  require(ptx.find("add.u32 %r16, %r30, 60;") != std::string::npos,
          "entry prologue should read KNL_LDS_NON_STACK_SIZE");
  require(count_substr(ptx, "add.u32 %r15, %r15, %r18;") >= 2,
          "entry prologue should add LDS non-stack prefix to x2 and x8");
  require(ptx.find("@%p0 st.param.u32") == std::string::npos,
          "mutable-state ABI must not predicate st.param");
  require(ptx.find("@%p0 ld.param.u32") == std::string::npos,
          "mutable-state ABI must not predicate ld.param");
  require(ptx.find("shfl.sync.idx.b32 %r31, %x1, %r2, 0x1f, %r1;") == std::string::npos,
          "replicated scalar-state call ABI must not broadcast xreg payload from the leader");
  require(ptx.find("@%p0 mov.u32 %x1") == std::string::npos,
          "replicated scalar-state call setup should not leave x1 in leader-only form");

  std::cout << "ok ptx helper call prototype\n";
  return 0;
}
