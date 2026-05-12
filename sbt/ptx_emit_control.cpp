#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

bool try_emit_control(EmitCtx &ctx, const sbt::cfg::BundleInst &bi) {
  const sbt::DecodedInst &di = bi.inst;
  const uint32_t pc = di.pc;
  const uint32_t bundle_pc = bi.pc;
  const uint32_t inst_pc = bi.inst_pc;

  if (di.emit.domain == EmitDomain::StructuredControl) {
    if (di.emit.structured_control_kind == StructuredControlKind::SetRpc ||
        di.emit.structured_control_kind == StructuredControlKind::Join ||
        di.emit.structured_control_kind == StructuredControlKind::Vsetvli) {
      return true;
    }
    if (di.emit.structured_control_kind == StructuredControlKind::EndPrg) {
      if (ctx.is_entry) {
        ctx.emit_entry_pds_pool_release(pc);
      }
      if (!ctx.is_entry) ctx.emit_store_mutable_state_blob("__sbt_mutable_state_out");
      ctx.emit_line("ret;");
      return true;
    }
    if (di.emit.structured_control_kind == StructuredControlKind::Barrier) {
      ctx.emit_line("bar.sync 0;");
      return true;
    }
  }

  if (is_ret(di)) {
    if (ctx.is_entry) {
      ctx.emit_entry_pds_pool_release(pc);
    }
    if (!ctx.is_entry) ctx.emit_store_mutable_state_blob("__sbt_mutable_state_out");
    ctx.emit_line("ret;");
    return true;
  }

  if (is_call(di)) {
    const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
    auto it = ctx.sym_by_addr.find(target);
    require(it != ctx.sym_by_addr.end(), EmitError("unsupported.call", ctx.func_name, pc, "target=" + hex_u32(target)));
    const std::string &callee = it->second;

    const std::string ret_addr = ctx.tmp_b32();
    ctx.emit_line("mov.u32 " + ret_addr + ", " + hex_u32(inst_pc + 4) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, ret_addr, pc);

    if (const auto builtin = lookup_builtin_call(callee)) {
      emit_builtin_call(ctx, *builtin, pc);
      return true;
    }
    require(!is_work_group_broadcast_call_name(callee),
            EmitError("unsupported.work_group_broadcast", ctx.func_name, pc, "callee=" + callee));

    require(ctx.mod.ptx_name_by_addr != nullptr, EmitError("unsupported.call", ctx.func_name, pc, "callee=" + callee));
    auto jt = ctx.mod.ptx_name_by_addr->find(target);
    require(jt != ctx.mod.ptx_name_by_addr->end(), EmitError("unsupported.call", ctx.func_name, pc, "callee=" + callee));
    ctx.emit_direct_call(jt->second);
    return true;
  }

  if (is_uncond_jump(di)) {
    const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
    const uint32_t src_block = ctx.block_start_of_pc(bundle_pc);
    const uint32_t dst_block = ctx.block_start_of_pc(target);
    ctx.emit_line("bra " + ctx.target_label_for_edge(src_block, dst_block) + ";");
    return true;
  }

  if (is_scalar_branch(di) && di.imm_kind == sbt::ImmKind::B13) {
    ctx.require_uniform_pure_scalar(di, pc);
    const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
    const uint32_t fallthrough = inst_pc + 4;
    const uint32_t src_block = ctx.block_start_of_pc(bundle_pc);
    const uint32_t dst_t = ctx.block_start_of_pc(target);
    const uint32_t dst_f = ctx.block_start_of_pc(fallthrough);

    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_all(r(15), di.rs2, pc);

    switch (di.emit.branch_cond) {
    case BranchCondKind::Eq: ctx.emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
    case BranchCondKind::Ne: ctx.emit_line("setp.ne.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
    case BranchCondKind::Lt: ctx.emit_line("setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
    case BranchCondKind::Ge: ctx.emit_line("setp.ge.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
    case BranchCondKind::Ltu: ctx.emit_line("setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
    case BranchCondKind::Geu: ctx.emit_line("setp.ge.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
    case BranchCondKind::None: throw EmitError("unsupported.inst", ctx.func_name, pc, di.name);
    }

    ctx.emit_line("@" + p(1) + " bra.uni " + ctx.target_label_for_edge(src_block, dst_t) + ";");
    ctx.emit_line("bra.uni " + ctx.target_label_for_edge(src_block, dst_f) + ";");
    return true;
  }

  if (is_vector_branch(di) && di.imm_kind == sbt::ImmKind::B13) {
    const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
    const uint32_t fallthrough = inst_pc + 4;
    const uint32_t src_block = ctx.block_start_of_pc(bundle_pc);
    const uint32_t dst_t = ctx.block_start_of_pc(target);
    const uint32_t dst_f = ctx.block_start_of_pc(fallthrough);
    const std::string a = v(di.rs2);
    const std::string b = v(di.rs1);

    switch (di.emit.branch_cond) {
    case BranchCondKind::Eq: ctx.emit_line("setp.eq.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
    case BranchCondKind::Ne: ctx.emit_line("setp.ne.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
    case BranchCondKind::Lt: ctx.emit_line("setp.lt.s32 " + p(1) + ", " + a + ", " + b + ";"); break;
    case BranchCondKind::Ge: ctx.emit_line("setp.ge.s32 " + p(1) + ", " + a + ", " + b + ";"); break;
    case BranchCondKind::Ltu: ctx.emit_line("setp.lt.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
    case BranchCondKind::Geu: ctx.emit_line("setp.ge.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
    case BranchCondKind::None: throw EmitError("unsupported.inst", ctx.func_name, pc, di.name);
    }

    ctx.emit_line("@" + p(1) + " bra " + ctx.target_label_for_edge(src_block, dst_t) + ";");
    ctx.emit_line("bra " + ctx.target_label_for_edge(src_block, dst_f) + ";");
    return true;
  }

  return false;
}

} // namespace sbt::ptx::detail
