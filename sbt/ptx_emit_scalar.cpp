#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

bool try_emit_scalar(EmitCtx &ctx, const sbt::DecodedInst &di) {
  const uint32_t pc = di.pc;

  if (di.emit.domain == EmitDomain::Csr) {
    ctx.require_uniform_pure_scalar(di, pc);
    require(di.imm_kind == sbt::ImmKind::CSR12, EmitError("invalid.csr", ctx.func_name, pc, "imm_kind"));
    const uint32_t csr = static_cast<uint32_t>(di.imm);

    if (csr == 0x803u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", " + r(30) + ";");
    } else if (csr == 0x802u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", 32;");
    } else if (csr == 0x805u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", " + r(10) + ";");
    } else if (csr == 0x800u) {
      ctx.emit_line(ctx.scalar_prefix() + "shl.b32 " + r(14) + ", " + r(10) + ", 5;");
    } else if (csr == 0x801u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", " + r(12) + ";");
    } else if (csr == 0x806u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(ctx.opt.shared_base_vaddr) + ";");
    } else if (csr == 0x807u) {
      ctx.emit_compute_csr_pds_u32(r(14), /*scalar=*/true);
    } else if (csr == 0x808u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.x;");
    } else if (csr == 0x809u) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.y;");
    } else if (csr == 0x80au) {
      ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.z;");
    } else if (csr == 0x80bu) {
      ctx.emit_load_knl_u32_scalar(r(14), kKnlPrintAddrOffset, pc);
    } else {
      throw EmitError("unsupported.csr", ctx.func_name, pc, "csr=" + hex_u32(csr));
    }

    ctx.emit_st_x_u32_scalar(di.rd, r(14), pc);
    return true;
  }

  if (is_scalar_memory(di, MemAccessKind::Load) && di.imm_kind == sbt::ImmKind::I12) {
    ctx.require_uniform_pure_scalar(di, pc);
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "add.u32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");

    if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::Integer) {
      ctx.emit_addr_map_and_ld_u32_scalar(r(17), r(15), pc);
      ctx.emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::FloatBits) {
      ctx.emit_addr_map_and_ld_u32_scalar(r(17), r(15), pc);
      ctx.emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
      ctx.emit_addr_map_and_ld_u8_zext_u32_scalar(r(17), r(15), pc);
      ctx.emit_line(ctx.scalar_prefix() + "shl.b32 " + r(17) + ", " + r(17) + ", 24;");
      ctx.emit_line(ctx.scalar_prefix() + "shr.s32 " + r(17) + ", " + r(17) + ", 24;");
      ctx.emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
      ctx.emit_addr_map_and_ld_u16_zext_u32_scalar(r(17), r(15), pc);
      ctx.emit_line(ctx.scalar_prefix() + "shl.b32 " + r(17) + ", " + r(17) + ", 16;");
      ctx.emit_line(ctx.scalar_prefix() + "shr.s32 " + r(17) + ", " + r(17) + ", 16;");
      ctx.emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
      ctx.emit_addr_map_and_ld_u8_zext_u32_scalar(r(17), r(15), pc);
      ctx.emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
      ctx.emit_addr_map_and_ld_u16_zext_u32_scalar(r(17), r(15), pc);
      ctx.emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }
    throw EmitError("unsupported.inst", ctx.func_name, pc, di.name);
  }

  if (is_scalar_memory(di, MemAccessKind::Store) && di.imm_kind == sbt::ImmKind::S12) {
    ctx.require_scalar_exec_kind(di, ScalarExecKind::ExternallySideEffecting, pc);
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "add.u32 " + r(16) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");

    if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::Integer) {
      ctx.emit_addr_map_and_st_u32_scalar(r(16), r(15), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::FloatBits) {
      ctx.emit_addr_map_and_st_u32_scalar(r(16), r(15), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Byte) {
      ctx.emit_line(ctx.scalar_prefix() + "cvt.u8.u32 " + u8(1) + ", " + r(15) + ";");
      ctx.emit_addr_map_and_st_u8_scalar(r(16), u8(1), pc);
      return true;
    }
    if (di.emit.mem_width == MemWidth::Half) {
      ctx.emit_line(ctx.scalar_prefix() + "cvt.u16.u32 " + u16(1) + ", " + r(15) + ";");
      ctx.emit_addr_map_and_st_u16_scalar(r(16), u16(1), pc);
      return true;
    }
    throw EmitError("unsupported.inst", ctx.func_name, pc, di.name);
  }

  if (ctx.try_emit_scalar_fp(di)) return true;

  if (!is_scalar_int(di)) return false;
  ctx.require_uniform_pure_scalar(di, pc);

  if (di.emit.scalar_int_kind == ScalarIntKind::Add && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "add.s32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Add && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "add.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Sub) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "sub.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::And && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "and.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Or && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "or.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Xor && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "xor.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::And && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "and.b32 " + r(15) + ", " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Or && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "or.b32 " + r(15) + ", " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Mul) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "mul.lo.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::MulH) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "mul.hi.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::MulHU) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "mul.hi.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::MulHSU) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "cvt.s64.s32 " + rd(18) + ", " + r(14) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "cvt.u64.u32 " + rd(19) + ", " + r(15) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "mul.lo.s64 " + rd(18) + ", " + rd(18) + ", " + rd(19) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
    ctx.emit_line(ctx.scalar_prefix() + "cvt.u32.u64 " + r(16) + ", " + rd(18) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Div || di.emit.scalar_int_kind == ScalarIntKind::DivU ||
      di.emit.scalar_int_kind == ScalarIntKind::Rem || di.emit.scalar_int_kind == ScalarIntKind::RemU) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);

    const bool signed_div = di.emit.scalar_int_kind == ScalarIntKind::Div || di.emit.scalar_int_kind == ScalarIntKind::Rem;
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + r(15) + ", 0;");
    if (signed_div) {
      ctx.emit_line("setp.eq.u32 " + p(2) + ", " + r(14) + ", 0x80000000;");
      ctx.emit_line("setp.eq.u32 " + p(3) + ", " + r(15) + ", 0xffffffff;");
      ctx.emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
    } else {
      ctx.emit_line("setp.ne.u32 " + p(2) + ", 0, 0;");
    }

    ctx.emit_line("not.pred " + p(4) + ", " + p(1) + ";");
    ctx.emit_line("not.pred " + p(5) + ", " + p(2) + ";");
    ctx.emit_line("and.pred " + p(4) + ", " + p(4) + ", " + p(5) + ";");

    if (di.emit.scalar_int_kind == ScalarIntKind::Div) {
      ctx.emit_line("mov.u32 " + r(16) + ", 0xffffffff;");
      ctx.emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0x80000000;");
      ctx.emit_line("@" + p(4) + " div.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    } else if (di.emit.scalar_int_kind == ScalarIntKind::DivU) {
      ctx.emit_line("mov.u32 " + r(16) + ", 0xffffffff;");
      ctx.emit_line("@" + p(4) + " div.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    } else if (di.emit.scalar_int_kind == ScalarIntKind::Rem) {
      ctx.emit_line("mov.u32 " + r(16) + ", " + r(14) + ";");
      ctx.emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0;");
      ctx.emit_line("@" + p(4) + " rem.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    } else {
      ctx.emit_line("mov.u32 " + r(16) + ", " + r(14) + ";");
      ctx.emit_line("@" + p(4) + " rem.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
    }

    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Lui && di.imm_kind == sbt::ImmKind::U20) {
    ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(14), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Auipc && di.imm_kind == sbt::ImmKind::U20) {
    const uint32_t val = static_cast<uint32_t>(di.pc + static_cast<uint32_t>(di.imm));
    ctx.emit_line(ctx.scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(val) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(14), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "shl.b32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if ((di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical || di.emit.scalar_int_kind == ScalarIntKind::ShiftRightArithmetic) &&
      di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    if (di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical) {
      ctx.emit_line(ctx.scalar_prefix() + "shr.u32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
    } else {
      ctx.emit_line(ctx.scalar_prefix() + "shr.s32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
    }
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 31;");
    ctx.emit_line(ctx.scalar_prefix() + "shl.b32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if ((di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical || di.emit.scalar_int_kind == ScalarIntKind::ShiftRightArithmetic) &&
      di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 31;");
    if (di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical) {
      ctx.emit_line(ctx.scalar_prefix() + "shr.u32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
    } else {
      ctx.emit_line(ctx.scalar_prefix() + "shr.s32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
    }
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::Xor && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "xor.b32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThan && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThanU && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_ld_x_u32_scalar(r(15), di.rs2, pc);
    ctx.emit_line(ctx.scalar_prefix() + "setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(16), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThan && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    ctx.emit_line(ctx.scalar_prefix() + "setp.lt.s32 " + p(1) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "selp.u32 " + r(15) + ", 1, 0, " + p(1) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }
  if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThanU && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_ld_x_u32_scalar(r(14), di.rs1, pc);
    const uint32_t imm_u = static_cast<uint32_t>(di.imm);
    ctx.emit_line(ctx.scalar_prefix() + "setp.lt.u32 " + p(1) + ", " + r(14) + ", " + hex_u32(imm_u) + ";");
    ctx.emit_line(ctx.scalar_prefix() + "selp.u32 " + r(15) + ", 1, 0, " + p(1) + ";");
    ctx.emit_st_x_u32_scalar(di.rd, r(15), pc);
    return true;
  }

  return false;
}

} // namespace sbt::ptx::detail
