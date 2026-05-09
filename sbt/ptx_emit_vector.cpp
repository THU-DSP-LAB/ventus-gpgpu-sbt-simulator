#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

static void emit_pds_addr(EmitCtx &ctx, const sbt::DecodedInst &di, uint32_t pc, bool include_byte_offset) {
  ctx.emit_line("add.s32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
  if (include_byte_offset) {
    ctx.emit_line("and.b32 " + r(17) + ", " + r(14) + ", 3;");
  }
  ctx.emit_line("and.b32 " + r(15) + ", " + r(14) + ", 0xfffffffc;");
  ctx.emit_line("mul.lo.u32 " + r(15) + ", " + r(15) + ", " + r(12) + ";");
  ctx.emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 5;");
  ctx.emit_line("shl.b32 " + r(16) + ", " + r(10) + ", 5;");
  ctx.emit_line("add.u32 " + r(16) + ", " + r(16) + ", " + r(0) + ";");
  ctx.emit_line("shl.b32 " + r(16) + ", " + r(16) + ", 2;");
  ctx.emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + r(16) + ";");
  ctx.emit_compute_csr_pds_u32(r(24), /*scalar=*/false);
  ctx.emit_line("add.u32 " + r(14) + ", " + r(24) + ", " + r(15) + ";");
  if (include_byte_offset) {
    ctx.emit_line("add.u32 " + r(14) + ", " + r(14) + ", " + r(17) + ";");
  }
  (void)pc;
}

static bool try_emit_vector_memory_and_register(EmitCtx &ctx, const sbt::DecodedInst &di) {
  const uint32_t pc = di.pc;

  if (is_vector_memory(di, MemAccessKind::Load, MemoryAddrKind::Pds) && di.imm_kind == sbt::ImmKind::I12) {
    emit_pds_addr(ctx, di, pc, /*include_byte_offset=*/false);
    ctx.emit_addr_map_and_ld_u32(r(20), r(14), pc);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(20) + ";");
    return true;
  }
  if (is_vector_memory(di, MemAccessKind::Store, MemoryAddrKind::Pds) && di.imm_kind == sbt::ImmKind::S12) {
    emit_pds_addr(ctx, di, pc, /*include_byte_offset=*/di.emit.mem_width == MemWidth::Byte);
    if (di.emit.mem_width == MemWidth::Byte) {
      ctx.emit_line("cvt.u8.u32 " + u8(1) + ", " + v(di.rs2) + ";");
      ctx.emit_addr_map_and_st_u8(r(14), u8(1), pc);
    } else {
      ctx.emit_addr_map_and_st_u32(r(14), v(di.rs2), pc);
    }
    return true;
  }

  if (is_vector_memory(di, MemAccessKind::Load, MemoryAddrKind::Ordinary) && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_line("add.u32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
    if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
      ctx.emit_addr_map_and_ld_u8_zext_u32(r(15), r(14), pc);
      ctx.emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 24;");
      ctx.emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 24;");
      ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    } else if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
      ctx.emit_addr_map_and_ld_u8_zext_u32(r(15), r(14), pc);
      ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    } else if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
      ctx.emit_addr_map_and_ld_u16_zext_u32(r(15), r(14), pc);
      ctx.emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 16;");
      ctx.emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 16;");
      ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    } else if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
      ctx.emit_addr_map_and_ld_u16_zext_u32(r(15), r(14), pc);
      ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    } else {
      ctx.emit_addr_map_and_ld_u32(r(15), r(14), pc);
      ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    }
    return true;
  }

  if (is_vector_memory(di, MemAccessKind::Store, MemoryAddrKind::Ordinary) && di.imm_kind == sbt::ImmKind::S12) {
    ctx.emit_line("add.u32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
    if (di.emit.mem_width == MemWidth::Byte) {
      ctx.emit_line("cvt.u8.u32 " + u8(1) + ", " + v(di.rs2) + ";");
      ctx.emit_addr_map_and_st_u8(r(14), u8(1), pc);
    } else if (di.emit.mem_width == MemWidth::Half) {
      ctx.emit_line("cvt.u16.u32 " + u16(1) + ", " + v(di.rs2) + ";");
      ctx.emit_addr_map_and_st_u16(r(14), u16(1), pc);
    } else {
      ctx.emit_addr_map_and_st_u32(r(14), v(di.rs2), pc);
    }
    return true;
  }

  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::BroadcastScalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::InsertScalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::BroadcastImmediate) {
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::MoveVector) {
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::ExtractScalar) {
    ctx.require_scalar_exec_kind(di, ScalarExecKind::FixedLaneSensitive, pc);
    ctx.emit_trap_if_lane_inactive(/*lane=*/0u);
    ctx.emit_line("setp.eq.u32 " + p(2) + ", " + r(0) + ", 0;");
    ctx.emit_line("@" + p(2) + " mov.u32 " + x(di.rd) + ", " + v(di.rs2) + ";");
    ctx.emit_read_activemask(r(1));
    ctx.emit_line("shfl.sync.idx.b32 " + x(di.rd) + ", " + x(di.rd) + ", 0, 0x1f, " + r(1) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::LaneId) {
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(0) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
      di.emit.merge_kind == MergeKind::Vvm) {
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + p(1) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
      di.emit.merge_kind == MergeKind::Vxm) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + p(1) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
      di.emit.merge_kind == MergeKind::Vim) {
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", " + std::to_string(di.imm) + ", " + v(di.rs2) + ", " + p(1) + ";");
    return true;
  }
  if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
      di.emit.merge_kind == MergeKind::Vfm) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + p(1) + ";");
    return true;
  }

  return false;
}

static bool try_emit_vector_integer(EmitCtx &ctx, const sbt::DecodedInst &di) {
  const uint32_t pc = di.pc;

  if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("add.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("add.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("add.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs1VectorImm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_line("add.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::RSub && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("sub.s32 " + v(di.rd) + ", " + std::to_string(di.imm) + ", " + v(di.rs2) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::RSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MinU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("min.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MinU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("min.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("min.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("min.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MaxU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("max.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MaxU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("max.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("max.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("max.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Sub && di.operand_form == sbt::OperandForm::VRdRs1VectorImm && di.imm_kind == sbt::ImmKind::I12) {
    ctx.emit_line("sub.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::And && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::And && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::And && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Or && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Or && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Or && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Xor && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Xor && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Xor && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
    ctx.emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
    ctx.emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftRightLogical && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftRightLogical && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
    ctx.emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftRightLogical && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
    ctx.emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftRightArithmetic && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftRightArithmetic && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
    ctx.emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::ShiftRightArithmetic && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
    ctx.emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mul.lo.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mul.lo.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MulH && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mul.hi.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MulH && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mul.hi.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MulHU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mul.hi.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MulHU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mul.hi.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MulHSU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("cvt.s64.s32 " + rd(16) + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.s64.u32 " + rd(17) + ", " + r(14) + ";");
    ctx.emit_line("mul.lo.s64 " + rd(18) + ", " + rd(16) + ", " + rd(17) + ";");
    ctx.emit_line("shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
    ctx.emit_line("cvt.u32.s64 " + r(15) + ", " + rd(18) + ";");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MulHSU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("cvt.s64.s32 " + rd(16) + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.s64.u32 " + rd(17) + ", " + v(di.rs1) + ";");
    ctx.emit_line("mul.lo.s64 " + rd(18) + ", " + rd(16) + ", " + rd(17) + ";");
    ctx.emit_line("shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
    ctx.emit_line("cvt.u32.s64 " + r(15) + ", " + rd(18) + ";");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::DivU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
    ctx.emit_line("@!" + p(1) + " div.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::DivU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
    ctx.emit_line("@!" + p(1) + " div.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::RemU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
    ctx.emit_line("@!" + p(1) + " rem.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::RemU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
    ctx.emit_line("@!" + p(1) + " rem.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
    ctx.emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
    ctx.emit_line("setp.eq.u32 " + p(3) + ", " + r(14) + ", 0xffffffff;");
    ctx.emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
    ctx.emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
    ctx.emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0x80000000;");
    ctx.emit_line("@!" + p(4) + " div.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
    ctx.emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
    ctx.emit_line("setp.eq.u32 " + p(3) + ", " + v(di.rs1) + ", 0xffffffff;");
    ctx.emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
    ctx.emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
    ctx.emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0x80000000;");
    ctx.emit_line("@!" + p(4) + " div.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Rem && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
    ctx.emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
    ctx.emit_line("setp.eq.u32 " + p(3) + ", " + r(14) + ", 0xffffffff;");
    ctx.emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
    ctx.emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
    ctx.emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0;");
    ctx.emit_line("@!" + p(4) + " rem.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Rem && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
    ctx.emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
    ctx.emit_line("setp.eq.u32 " + p(3) + ", " + v(di.rs1) + ", 0xffffffff;");
    ctx.emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
    ctx.emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
    ctx.emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
    ctx.emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0;");
    ctx.emit_line("@!" + p(4) + " rem.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Madd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::Madd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mul.lo.u32 " + r(14) + ", " + v(di.rd) + ", " + v(di.rs1) + ";");
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mul.lo.u32 " + r(15) + ", " + v(di.rd) + ", " + r(14) + ";");
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(15) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mad.lo.s32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mul.lo.s32 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_int_kind == VectorIntKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mul.lo.s32 " + r(15) + ", " + r(14) + ", " + v(di.rs2) + ";");
    ctx.emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(15) + ";");
    return true;
  }

  return false;
}

static bool try_emit_vector_compare_and_convert(EmitCtx &ctx, const sbt::DecodedInst &di) {
  const uint32_t pc = di.pc;
  auto emit_mask_from_pred = [&](const std::string &pred) { ctx.emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + pred + ";"); };

  if (di.emit.vector_compare_kind == VectorCompareKind::Eq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Eq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Eq && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Ne && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Ne && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Ne && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::LtU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.lt.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Lt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.lt.s32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::LeU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::LeU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::LeU && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Le && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Le && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::GtU && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("setp.gt.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::GtU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.gt.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Gt && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("setp.gt.s32 " + p(1) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Gt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.gt.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }

  if (di.emit.domain == EmitDomain::VectorMask) {
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", 0;");
    ctx.emit_line("setp.ne.u32 " + p(2) + ", " + v(di.rs1) + ", 0;");
    const bool invert_b = di.emit.vector_mask_kind == VectorMaskKind::AndNot || di.emit.vector_mask_kind == VectorMaskKind::OrNot;
    if (invert_b) ctx.emit_line("not.pred " + p(2) + ", " + p(2) + ";");
    if (di.emit.vector_mask_kind == VectorMaskKind::And || di.emit.vector_mask_kind == VectorMaskKind::AndNot) {
      ctx.emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
    } else if (di.emit.vector_mask_kind == VectorMaskKind::Or || di.emit.vector_mask_kind == VectorMaskKind::OrNot) {
      ctx.emit_line("or.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
    } else {
      ctx.emit_line("xor.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
    }
    if (di.emit.vector_mask_kind == VectorMaskKind::XNor || di.emit.vector_mask_kind == VectorMaskKind::Nand ||
        di.emit.vector_mask_kind == VectorMaskKind::Nor) {
      ctx.emit_line("not.pred " + p(3) + ", " + p(3) + ";");
    }
    emit_mask_from_pred(p(3));
    return true;
  }

  auto emit_vf_cmp_vv = [&](const char *cmp) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs1) + ";");
    ctx.emit_line(std::string(cmp) + " " + p(1) + ", " + f(0) + ", " + f(1) + ";");
    emit_mask_from_pred(p(1));
  };
  auto emit_vf_cmp_vf = [&](const char *cmp) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + r(14) + ";");
    ctx.emit_line(std::string(cmp) + " " + p(1) + ", " + f(0) + ", " + f(1) + ";");
    emit_mask_from_pred(p(1));
  };
  if (di.emit.vector_compare_kind == VectorCompareKind::FEq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { emit_vf_cmp_vv("setp.eq.f32"); return true; }
  if (di.emit.vector_compare_kind == VectorCompareKind::FEq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { emit_vf_cmp_vf("setp.eq.f32"); return true; }
  if (di.emit.vector_compare_kind == VectorCompareKind::FLe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { emit_vf_cmp_vv("setp.le.f32"); return true; }
  if (di.emit.vector_compare_kind == VectorCompareKind::FLe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { emit_vf_cmp_vf("setp.le.f32"); return true; }
  if (di.emit.vector_compare_kind == VectorCompareKind::FLt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { emit_vf_cmp_vv("setp.lt.f32"); return true; }
  if (di.emit.vector_compare_kind == VectorCompareKind::FLt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { emit_vf_cmp_vf("setp.lt.f32"); return true; }
  if (di.emit.vector_compare_kind == VectorCompareKind::FGt) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
    ctx.emit_line("setp.lt.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::FGe) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
    ctx.emit_line("setp.le.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::FNe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    emit_vf_cmp_vv("setp.eq.f32");
    ctx.emit_line("not.pred " + p(1) + ", " + p(1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::FNe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    emit_vf_cmp_vf("setp.eq.f32");
    ctx.emit_line("not.pred " + p(1) + ", " + p(1) + ";");
    emit_mask_from_pred(p(1));
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Lt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.lt.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::LtU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("setp.lt.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
    return true;
  }
  if (di.emit.vector_compare_kind == VectorCompareKind::Le && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
    ctx.emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
    ctx.emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
    return true;
  }

  if (di.emit.vector_convert_kind == VectorConvertKind::FloatFromInt) {
    ctx.emit_line("cvt.rn.f32.s32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
    return true;
  }
  if (di.emit.vector_convert_kind == VectorConvertKind::FloatFromUInt) {
    ctx.emit_line("cvt.rn.f32.u32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
    return true;
  }
  if (di.emit.vector_convert_kind == VectorConvertKind::IntFromFloat) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.rni.s32.f32 " + r(14) + ", " + f(0) + ";");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_convert_kind == VectorConvertKind::UIntFromFloat) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.rni.u32.f32 " + r(14) + ", " + f(0) + ";");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_convert_kind == VectorConvertKind::IntFromFloatRtz) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.rzi.s32.f32 " + r(14) + ", " + f(0) + ";");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_convert_kind == VectorConvertKind::UIntFromFloatRtz) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.rzi.u32.f32 " + r(14) + ", " + f(0) + ";");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
    return true;
  }
  if (di.emit.vector_convert_kind == VectorConvertKind::Classify) {
    ctx.emit_line("mov.u32 " + r(14) + ", " + v(di.rs2) + ";");
    ctx.emit_line("and.b32 " + r(15) + ", " + r(14) + ", 0x80000000;");
    ctx.emit_line("and.b32 " + r(16) + ", " + r(14) + ", 0x7f800000;");
    ctx.emit_line("and.b32 " + r(17) + ", " + r(14) + ", 0x007fffff;");
    ctx.emit_line("setp.ne.u32 " + p(1) + ", " + r(15) + ", 0;");
    ctx.emit_line("setp.eq.u32 " + p(2) + ", " + r(16) + ", 0;");
    ctx.emit_line("setp.eq.u32 " + p(3) + ", " + r(16) + ", 0x7f800000;");
    ctx.emit_line("setp.eq.u32 " + p(4) + ", " + r(17) + ", 0;");
    ctx.emit_line("and.pred " + p(5) + ", " + p(2) + ", " + p(4) + ";");
    ctx.emit_line("not.pred " + p(6) + ", " + p(4) + ";");
    ctx.emit_line("and.pred " + p(6) + ", " + p(2) + ", " + p(6) + ";");
    ctx.emit_line("and.pred " + p(7) + ", " + p(3) + ", " + p(4) + ";");
    ctx.emit_line("not.pred " + p(8) + ", " + p(4) + ";");
    ctx.emit_line("and.pred " + p(8) + ", " + p(3) + ", " + p(8) + ";");
    ctx.emit_line("not.pred " + p(9) + ", " + p(2) + ";");
    ctx.emit_line("not.pred " + p(10) + ", " + p(3) + ";");
    ctx.emit_line("and.pred " + p(9) + ", " + p(9) + ", " + p(10) + ";");
    ctx.emit_line("and.b32 " + r(18) + ", " + r(17) + ", 0x00400000;");
    ctx.emit_line("setp.ne.u32 " + p(10) + ", " + r(18) + ", 0;");
    ctx.emit_line("and.pred " + p(10) + ", " + p(8) + ", " + p(10) + ";");
    ctx.emit_line("not.pred " + p(11) + ", " + p(10) + ";");
    ctx.emit_line("and.pred " + p(11) + ", " + p(8) + ", " + p(11) + ";");
    ctx.emit_line("mov.u32 " + r(19) + ", 0;");
    ctx.emit_line("and.pred " + p(12) + ", " + p(7) + ", " + p(1) + ";");
    ctx.emit_line("not.pred " + p(13) + ", " + p(1) + ";");
    ctx.emit_line("and.pred " + p(13) + ", " + p(7) + ", " + p(13) + ";");
    ctx.emit_line("@" + p(12) + " or.b32 " + r(19) + ", " + r(19) + ", 1;");
    ctx.emit_line("@" + p(13) + " or.b32 " + r(19) + ", " + r(19) + ", 128;");
    ctx.emit_line("and.pred " + p(14) + ", " + p(5) + ", " + p(1) + ";");
    ctx.emit_line("not.pred " + p(15) + ", " + p(1) + ";");
    ctx.emit_line("and.pred " + p(15) + ", " + p(5) + ", " + p(15) + ";");
    ctx.emit_line("@" + p(14) + " or.b32 " + r(19) + ", " + r(19) + ", 8;");
    ctx.emit_line("@" + p(15) + " or.b32 " + r(19) + ", " + r(19) + ", 16;");
    ctx.emit_line("and.pred " + p(12) + ", " + p(6) + ", " + p(1) + ";");
    ctx.emit_line("not.pred " + p(13) + ", " + p(1) + ";");
    ctx.emit_line("and.pred " + p(13) + ", " + p(6) + ", " + p(13) + ";");
    ctx.emit_line("@" + p(12) + " or.b32 " + r(19) + ", " + r(19) + ", 4;");
    ctx.emit_line("@" + p(13) + " or.b32 " + r(19) + ", " + r(19) + ", 32;");
    ctx.emit_line("and.pred " + p(12) + ", " + p(9) + ", " + p(1) + ";");
    ctx.emit_line("not.pred " + p(13) + ", " + p(1) + ";");
    ctx.emit_line("and.pred " + p(13) + ", " + p(9) + ", " + p(13) + ";");
    ctx.emit_line("@" + p(12) + " or.b32 " + r(19) + ", " + r(19) + ", 2;");
    ctx.emit_line("@" + p(13) + " or.b32 " + r(19) + ", " + r(19) + ", 64;");
    ctx.emit_line("@" + p(11) + " or.b32 " + r(19) + ", " + r(19) + ", 256;");
    ctx.emit_line("@" + p(10) + " or.b32 " + r(19) + ", " + r(19) + ", 512;");
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + r(19) + ";");
    return true;
  }

  return false;
}

static bool try_emit_vector_fp(EmitCtx &ctx, const sbt::DecodedInst &di) {
  const uint32_t pc = di.pc;

  if (di.emit.vector_fp_kind == VectorFpKind::Exp) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mul.rn.f32 " + f(1) + ", " + f(0) + ", 1.4426950408889634;");
    ctx.emit_line("ex2.approx.f32 " + f(2) + ", " + f(1) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
    return true;
  }

  auto vf_binop = [&](const char *op) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs1) + ";");
    ctx.emit_line(std::string(op) + " " + f(2) + ", " + f(0) + ", " + f(1) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
  };
  auto vf_binop_vf = [&](const char *op) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + r(14) + ";");
    ctx.emit_line(std::string(op) + " " + f(2) + ", " + f(0) + ", " + f(1) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
  };

  if (di.emit.vector_fp_kind == VectorFpKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("add.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("add.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("sub.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("sub.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("mul.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("mul.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("div.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("div.rn.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::RSub) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
    ctx.emit_line("sub.rn.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
    return true;
  }
  if (di.emit.vector_fp_kind == VectorFpKind::RDiv) {
    ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    ctx.emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
    ctx.emit_line("div.rn.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
    return true;
  }
  if (di.emit.vector_fp_kind == VectorFpKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("min.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("min.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("max.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("max.f32"); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rd) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + v(di.rs1) + ";");
    ctx.emit_line("mov.b32 " + f(2) + ", " + v(di.rs2) + ";");
    ctx.emit_line("fma.rn.f32 " + f(3) + ", " + f(0) + ", " + f(1) + ", " + f(2) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(3) + ";");
    return true;
  }
  auto vf_fma_bits = [&](const std::string &a_bits, bool neg_a, const std::string &b_bits, bool neg_b, const std::string &c_bits, bool neg_c) {
    ctx.emit_line("mov.u32 " + r(14) + ", " + a_bits + ";");
    ctx.emit_line("mov.u32 " + r(15) + ", " + b_bits + ";");
    ctx.emit_line("mov.u32 " + r(16) + ", " + c_bits + ";");
    if (neg_a) ctx.emit_line("xor.b32 " + r(14) + ", " + r(14) + ", 0x80000000;");
    if (neg_b) ctx.emit_line("xor.b32 " + r(15) + ", " + r(15) + ", 0x80000000;");
    if (neg_c) ctx.emit_line("xor.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
    ctx.emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
    ctx.emit_line("mov.b32 " + f(1) + ", " + r(15) + ";");
    ctx.emit_line("mov.b32 " + f(2) + ", " + r(16) + ";");
    ctx.emit_line("fma.rn.f32 " + f(3) + ", " + f(0) + ", " + f(1) + ", " + f(2) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(3) + ";");
  };

  if (di.emit.vector_fp_kind == VectorFpKind::MAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(v(di.rd), false, r(15), false, v(di.rs2), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rd), false, v(di.rs1), false, v(di.rs2), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(v(di.rd), false, r(15), false, v(di.rs2), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rd), true, v(di.rs1), false, v(di.rs2), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(v(di.rd), true, r(15), false, v(di.rs2), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rd), true, v(di.rs1), false, v(di.rs2), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(v(di.rd), true, r(15), false, v(di.rs2), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rs1), false, v(di.rs2), false, v(di.rd), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(r(15), false, v(di.rs2), false, v(di.rd), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rs2), true, v(di.rs1), false, v(di.rd), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(r(15), false, v(di.rs2), true, v(di.rd), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rs1), false, v(di.rs2), false, v(di.rd), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::MSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(r(15), false, v(di.rs2), false, v(di.rd), true); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_fma_bits(v(di.rs1), true, v(di.rs2), false, v(di.rd), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { ctx.emit_ld_x_u32_all(r(15), di.rs1, pc); vf_fma_bits(r(15), false, v(di.rs2), true, v(di.rd), false); return true; }
  if (di.emit.vector_fp_kind == VectorFpKind::Sqrt) {
    ctx.emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
    ctx.emit_line("sqrt.rn.f32 " + f(1) + ", " + f(0) + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + f(1) + ";");
    return true;
  }
  if (di.emit.vector_fp_kind == VectorFpKind::SignInject) {
    if (di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      ctx.emit_ld_x_u32_all(r(14), di.rs1, pc);
    }
    const std::string sign_src = (di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) ? r(14) : v(di.rs1);
    ctx.emit_line("and.b32 " + r(15) + ", " + v(di.rs2) + ", 0x7fffffff;");
    if (di.emit.vector_fp_sign_inject_kind == VectorFpSignInjectKind::CopySign) {
      ctx.emit_line("and.b32 " + r(16) + ", " + sign_src + ", 0x80000000;");
    } else if (di.emit.vector_fp_sign_inject_kind == VectorFpSignInjectKind::NegateSign) {
      ctx.emit_line("not.b32 " + r(16) + ", " + sign_src + ";");
      ctx.emit_line("and.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
    } else {
      ctx.emit_line("xor.b32 " + r(16) + ", " + sign_src + ", " + v(di.rs2) + ";");
      ctx.emit_line("and.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
    }
    ctx.emit_line("or.b32 " + v(di.rd) + ", " + r(15) + ", " + r(16) + ";");
    return true;
  }

  return false;
}

bool try_emit_vector(EmitCtx &ctx, const sbt::DecodedInst &di) {
  if (try_emit_vector_memory_and_register(ctx, di)) return true;
  if (try_emit_vector_integer(ctx, di)) return true;
  if (try_emit_vector_compare_and_convert(ctx, di)) return true;
  if (try_emit_vector_fp(ctx, di)) return true;
  return false;
}

} // namespace sbt::ptx::detail
