#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

void EmitCtx::maybe_note_fp_dyn_rm(uint32_t pc_for_err) {
  if (!opt.include_comments) return;
  if (emitted_fp_dyn_note) return;
  emit_line("// NOTE: scalar FP rm=DYN treated as RNE (CSR.frm not modeled)");
  emitted_fp_dyn_note = true;
  (void)pc_for_err;
}

sbt::FpRoundingMode EmitCtx::normalize_fp_rm(sbt::FpRoundingMode rm, uint32_t pc_for_err) {
  if (rm == sbt::FpRoundingMode::None) return rm;
  if (rm == sbt::FpRoundingMode::DYN) {
    maybe_note_fp_dyn_rm(pc_for_err);
    return sbt::FpRoundingMode::RNE;
  }
  if (rm == sbt::FpRoundingMode::RMM || rm == sbt::FpRoundingMode::Reserved5 || rm == sbt::FpRoundingMode::Reserved6) {
    throw EmitError("unsupported.fp_rm", func_name, pc_for_err, "rm=" + std::string(sbt::to_string(rm)));
  }
  return rm;
}

std::string EmitCtx::ptx_rm_f32(sbt::FpRoundingMode rm, uint32_t pc_for_err) {
  rm = normalize_fp_rm(rm, pc_for_err);
  switch (rm) {
  case sbt::FpRoundingMode::RNE: return ".rn";
  case sbt::FpRoundingMode::RTZ: return ".rz";
  case sbt::FpRoundingMode::RDN: return ".rm";
  case sbt::FpRoundingMode::RUP: return ".rp";
  case sbt::FpRoundingMode::None: default: throw EmitError("invalid.fp_rm", func_name, pc_for_err, "rm=none");
  }
}

std::string EmitCtx::ptx_rm_cvt_i32(sbt::FpRoundingMode rm, uint32_t pc_for_err) {
  rm = normalize_fp_rm(rm, pc_for_err);
  switch (rm) {
  case sbt::FpRoundingMode::RNE: return ".rni";
  case sbt::FpRoundingMode::RTZ: return ".rzi";
  case sbt::FpRoundingMode::RDN: return ".rmi";
  case sbt::FpRoundingMode::RUP: return ".rpi";
  case sbt::FpRoundingMode::None: default: throw EmitError("invalid.fp_rm", func_name, pc_for_err, "rm=none");
  }
}

void EmitCtx::emit_scalar_fclass_s(const sbt::DecodedInst &di, uint32_t pc_for_err) {
  // RISC-V FCLASS.S: return a 10-bit class mask in rd (integer bits).
  // Bits: 0..9 = -inf, -norm, -subnorm, -0, +0, +subnorm, +norm, +inf, sNaN, qNaN
  const std::string bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc_for_err);
  const std::string sign_bits = tmp_b32();
  const std::string exp_bits = tmp_b32();
  const std::string frac_bits = tmp_b32();
  const std::string qnan_bit = tmp_b32();
  const std::string mask = tmp_b32();
  const std::string bit_mask = tmp_b32();
  const std::string is_sign = tmp_pred();
  const std::string is_exp_zero = tmp_pred();
  const std::string is_exp_all1 = tmp_pred();
  const std::string is_frac_zero = tmp_pred();
  const std::string is_zero = tmp_pred();
  const std::string is_sub = tmp_pred();
  const std::string is_inf = tmp_pred();
  const std::string is_nan = tmp_pred();
  const std::string is_norm = tmp_pred();
  const std::string is_qnan = tmp_pred();
  const std::string is_snan = tmp_pred();
  const std::string neg_class = tmp_pred();
  const std::string pos_class = tmp_pred();
  emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + bits + ", 0x80000000;");
  emit_line(scalar_prefix() + "and.b32 " + exp_bits + ", " + bits + ", 0x7f800000;");
  emit_line(scalar_prefix() + "and.b32 " + frac_bits + ", " + bits + ", 0x007fffff;");

  emit_line(scalar_prefix() + "setp.ne.u32 " + is_sign + ", " + sign_bits + ", 0;");
  emit_line(scalar_prefix() + "setp.eq.u32 " + is_exp_zero + ", " + exp_bits + ", 0;");
  emit_line(scalar_prefix() + "setp.eq.u32 " + is_exp_all1 + ", " + exp_bits + ", 0x7f800000;");
  emit_line(scalar_prefix() + "setp.eq.u32 " + is_frac_zero + ", " + frac_bits + ", 0;");

  emit_line(scalar_prefix() + "and.pred " + is_zero + ", " + is_exp_zero + ", " + is_frac_zero + ";");
  emit_line(scalar_prefix() + "not.pred " + is_sub + ", " + is_frac_zero + ";");
  emit_line(scalar_prefix() + "and.pred " + is_sub + ", " + is_exp_zero + ", " + is_sub + ";");
  emit_line(scalar_prefix() + "and.pred " + is_inf + ", " + is_exp_all1 + ", " + is_frac_zero + ";");
  emit_line(scalar_prefix() + "not.pred " + is_nan + ", " + is_frac_zero + ";");
  emit_line(scalar_prefix() + "and.pred " + is_nan + ", " + is_exp_all1 + ", " + is_nan + ";");
  emit_line(scalar_prefix() + "not.pred " + is_norm + ", " + is_exp_zero + ";");
  emit_line(scalar_prefix() + "not.pred " + is_qnan + ", " + is_exp_all1 + ";");
  emit_line(scalar_prefix() + "and.pred " + is_norm + ", " + is_norm + ", " + is_qnan + ";");

  emit_line(scalar_prefix() + "and.b32 " + qnan_bit + ", " + frac_bits + ", 0x00400000;");
  emit_line(scalar_prefix() + "setp.ne.u32 " + is_qnan + ", " + qnan_bit + ", 0;");
  emit_line(scalar_prefix() + "and.pred " + is_qnan + ", " + is_nan + ", " + is_qnan + ";");
  emit_line(scalar_prefix() + "not.pred " + is_snan + ", " + is_qnan + ";");
  emit_line(scalar_prefix() + "and.pred " + is_snan + ", " + is_nan + ", " + is_snan + ";");

  emit_line(scalar_prefix() + "mov.u32 " + mask + ", 0;");

  // Derive per-class predicates.
  // -inf / +inf
  emit_line(scalar_prefix() + "and.pred " + p(12) + ", " + p(7) + ", " + p(1) + ";"); // -inf
  emit_line(scalar_prefix() + "not.pred " + p(13) + ", " + p(1) + ";");
  emit_line(scalar_prefix() + "and.pred " + p(13) + ", " + p(7) + ", " + p(13) + ";"); // +inf
  // -0 / +0
  emit_line(scalar_prefix() + "and.pred " + p(14) + ", " + p(5) + ", " + p(1) + ";"); // -0
  emit_line(scalar_prefix() + "not.pred " + p(15) + ", " + p(1) + ";");
  emit_line(scalar_prefix() + "and.pred " + p(15) + ", " + p(5) + ", " + p(15) + ";"); // +0

  auto or_if = [&](const std::string &pred, uint32_t bit) {
    emit_line(scalar_prefix() + "selp.u32 " + bit_mask + ", " + std::to_string(bit) + ", 0, " + pred + ";");
    emit_line(scalar_prefix() + "or.b32 " + mask + ", " + mask + ", " + bit_mask + ";");
  };

  emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_inf + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_inf + ", " + pos_class + ";");
  or_if(neg_class, 1u);
  or_if(pos_class, 128u);

  emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_zero + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_zero + ", " + pos_class + ";");
  or_if(neg_class, 8u);
  or_if(pos_class, 16u);

  // -sub / +sub
  emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_sub + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_sub + ", " + pos_class + ";");
  or_if(neg_class, 4u);
  or_if(pos_class, 32u);

  // -norm / +norm
  emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_norm + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
  emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_norm + ", " + pos_class + ";");
  or_if(neg_class, 2u);
  or_if(pos_class, 64u);

  // NaNs
  or_if(is_snan, 256u);
  or_if(is_qnan, 512u);

  emit_st_x_u32_scalar(di.rd, mask, pc_for_err);
}

bool EmitCtx::try_emit_scalar_fp(const sbt::DecodedInst &di) {
  if (!is_scalar_fp(di)) return false;
  const uint32_t pc = di.pc;
  require_uniform_pure_scalar(di, pc);

  // Bitwise moves (Zfinx: both sides are X regs).
  if (di.emit.scalar_fp_kind == ScalarFpKind::MoveBits) {
    emit_st_x_u32_scalar(di.rd, emit_ld_x_u32_scalar_tmp(di.rs1, pc), pc);
    return true;
  }

  // Sign injection.
  if (di.emit.scalar_fp_kind == ScalarFpKind::SignInject) {
    const std::string magnitude = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string sign_src = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
    const std::string result = tmp_b32();
    const std::string sign_bits = tmp_b32();
    emit_line(scalar_prefix() + "and.b32 " + result + ", " + magnitude + ", 0x7fffffff;");
    if (di.emit.fp_sign_inject_kind == FpSignInjectKind::CopySign) {
      emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + sign_src + ", 0x80000000;");
    } else if (di.emit.fp_sign_inject_kind == FpSignInjectKind::NegateSign) {
      emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + sign_src + ", 0x80000000;");
      emit_line(scalar_prefix() + "xor.b32 " + sign_bits + ", " + sign_bits + ", 0x80000000;");
    } else { // fsgnjx_s
      emit_line(scalar_prefix() + "xor.b32 " + sign_bits + ", " + magnitude + ", " + sign_src + ";");
      emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + sign_bits + ", 0x80000000;");
    }
    emit_line(scalar_prefix() + "or.b32 " + result + ", " + result + ", " + sign_bits + ";");
    emit_st_x_u32_scalar(di.rd, result, pc);
    return true;
  }

  // Arithmetic.
  auto f32_binop = [&](const char *op) {
    const std::string rm = ptx_rm_f32(di.fp_rm, pc);
    const std::string lhs_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string rhs_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
    const std::string lhs = tmp_f32();
    const std::string rhs = tmp_f32();
    const std::string result = tmp_f32();
    const std::string result_bits = tmp_b32();
    emit_line(scalar_prefix() + "mov.b32 " + lhs + ", " + lhs_bits + ";");
    emit_line(scalar_prefix() + "mov.b32 " + rhs + ", " + rhs_bits + ";");
    emit_line(scalar_prefix() + std::string(op) + rm + ".f32 " + result + ", " + lhs + ", " + rhs + ";");
    emit_line(scalar_prefix() + "mov.b32 " + result_bits + ", " + result + ";");
    emit_st_x_u32_scalar(di.rd, result_bits, pc);
  };
  if (di.emit.scalar_fp_kind == ScalarFpKind::Binary) {
    switch (di.emit.fp_binary_kind) {
    case FpBinaryKind::Add: f32_binop("add"); return true;
    case FpBinaryKind::Sub: f32_binop("sub"); return true;
    case FpBinaryKind::Mul: f32_binop("mul"); return true;
    case FpBinaryKind::Div: f32_binop("div"); return true;
    case FpBinaryKind::None: break;
    }
  }
  if (di.emit.scalar_fp_kind == ScalarFpKind::Sqrt) {
    const std::string rm = ptx_rm_f32(di.fp_rm, pc);
    const std::string src_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string src = tmp_f32();
    const std::string dst = tmp_f32();
    const std::string dst_bits = tmp_b32();
    emit_line(scalar_prefix() + "mov.b32 " + src + ", " + src_bits + ";");
    emit_line(scalar_prefix() + "sqrt" + rm + ".f32 " + dst + ", " + src + ";");
    emit_line(scalar_prefix() + "mov.b32 " + dst_bits + ", " + dst + ";");
    emit_st_x_u32_scalar(di.rd, dst_bits, pc);
    return true;
  }

  // FMA family.
  if (di.emit.scalar_fp_kind == ScalarFpKind::Fma) {
    const std::string rm = ptx_rm_f32(di.fp_rm, pc);
    const std::string a_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string b_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
    const std::string c_bits = emit_ld_x_u32_scalar_tmp(di.rs3, pc);
    const std::string a = tmp_f32();
    const std::string b = tmp_f32();
    const std::string c = tmp_f32();
    const std::string dst = tmp_f32();
    const std::string dst_bits = tmp_b32();
    if (di.emit.fp_ternary_kind == FpTernaryKind::MSub || di.emit.fp_ternary_kind == FpTernaryKind::NMAdd) {
      emit_line(scalar_prefix() + "xor.b32 " + c_bits + ", " + c_bits + ", 0x80000000;");
    }
    if (di.emit.fp_ternary_kind == FpTernaryKind::NMSub || di.emit.fp_ternary_kind == FpTernaryKind::NMAdd) {
      emit_line(scalar_prefix() + "xor.b32 " + a_bits + ", " + a_bits + ", 0x80000000;");
    }
    emit_line(scalar_prefix() + "mov.b32 " + a + ", " + a_bits + ";");
    emit_line(scalar_prefix() + "mov.b32 " + b + ", " + b_bits + ";");
    emit_line(scalar_prefix() + "mov.b32 " + c + ", " + c_bits + ";");
    emit_line(scalar_prefix() + "fma" + rm + ".f32 " + dst + ", " + a + ", " + b + ", " + c + ";");
    emit_line(scalar_prefix() + "mov.b32 " + dst_bits + ", " + dst + ";");
    emit_st_x_u32_scalar(di.rd, dst_bits, pc);
    return true;
  }

  // Min/max (NaN-safe).
  if (di.emit.scalar_fp_kind == ScalarFpKind::MinMax) {
    const std::string lhs_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string rhs_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
    const std::string lhs = tmp_f32();
    const std::string rhs = tmp_f32();
    const std::string result = tmp_f32();
    const std::string lhs_nan = tmp_pred();
    const std::string rhs_nan = tmp_pred();
    const std::string result_bits = tmp_b32();
    emit_line(scalar_prefix() + "mov.b32 " + lhs + ", " + lhs_bits + ";");
    emit_line(scalar_prefix() + "mov.b32 " + rhs + ", " + rhs_bits + ";");
    emit_line(scalar_prefix() + "setp.nan.f32 " + lhs_nan + ", " + lhs + ", " + lhs + ";");
    emit_line(scalar_prefix() + "setp.nan.f32 " + rhs_nan + ", " + rhs + ", " + rhs + ";");
    emit_line(scalar_prefix() + std::string(di.emit.fp_minmax_kind == FpMinMaxKind::Max ? "max" : "min") + ".f32 " + result + ", " + lhs +
              ", " + rhs + ";");
    emit_line(scalar_prefix() + "selp.b32 " + result + ", " + rhs + ", " + result + ", " + lhs_nan + ";");
    emit_line(scalar_prefix() + "selp.b32 " + result + ", " + lhs + ", " + result + ", " + rhs_nan + ";");
    emit_line(scalar_prefix() + "mov.b32 " + result_bits + ", " + result + ";");
    emit_st_x_u32_scalar(di.rd, result_bits, pc);
    return true;
  }

  // Comparisons: exact 0/1 integer result.
  if (di.emit.scalar_fp_kind == ScalarFpKind::Compare) {
    const std::string lhs_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string rhs_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
    const std::string lhs = tmp_f32();
    const std::string rhs = tmp_f32();
    const std::string pred = tmp_pred();
    const std::string result = tmp_b32();
    emit_line(scalar_prefix() + "mov.b32 " + lhs + ", " + lhs_bits + ";");
    emit_line(scalar_prefix() + "mov.b32 " + rhs + ", " + rhs_bits + ";");
    if (di.emit.fp_compare_kind == FpCompareKind::Eq) emit_line(scalar_prefix() + "setp.eq.f32 " + pred + ", " + lhs + ", " + rhs + ";");
    else if (di.emit.fp_compare_kind == FpCompareKind::Lt) emit_line(scalar_prefix() + "setp.lt.f32 " + pred + ", " + lhs + ", " + rhs + ";");
    else emit_line(scalar_prefix() + "setp.le.f32 " + pred + ", " + lhs + ", " + rhs + ";");
    emit_line(scalar_prefix() + "selp.u32 " + result + ", 1, 0, " + pred + ";");
    emit_st_x_u32_scalar(di.rd, result, pc);
    return true;
  }

  // Conversions.
  if (di.emit.scalar_fp_kind == ScalarFpKind::Convert && (di.emit.fp_convert_kind == FpConvertKind::IntToFloatSigned ||
                                                           di.emit.fp_convert_kind == FpConvertKind::IntToFloatUnsigned)) {
    const std::string rm = ptx_rm_f32(di.fp_rm, pc);
    const std::string src = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string dst = tmp_f32();
    const std::string dst_bits = tmp_b32();
    emit_line(scalar_prefix() + "cvt" + rm + ".f32." +
              (di.emit.fp_convert_kind == FpConvertKind::IntToFloatSigned ? "s32 " : "u32 ") + dst + ", " + src + ";");
    emit_line(scalar_prefix() + "mov.b32 " + dst_bits + ", " + dst + ";");
    emit_st_x_u32_scalar(di.rd, dst_bits, pc);
    return true;
  }
  if (di.emit.scalar_fp_kind == ScalarFpKind::Convert && (di.emit.fp_convert_kind == FpConvertKind::FloatToIntSigned ||
                                                           di.emit.fp_convert_kind == FpConvertKind::FloatToIntUnsigned)) {
    const std::string rm = ptx_rm_cvt_i32(di.fp_rm, pc);
    const std::string src_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
    const std::string src = tmp_f32();
    const std::string dst = tmp_b32();
    emit_line(scalar_prefix() + "mov.b32 " + src + ", " + src_bits + ";");
    emit_line(scalar_prefix() + "cvt" + rm + "." +
              (di.emit.fp_convert_kind == FpConvertKind::FloatToIntSigned ? "s32" : "u32") + ".f32 " + dst + ", " + src + ";");
    emit_st_x_u32_scalar(di.rd, dst, pc);
    return true;
  }

  if (di.emit.scalar_fp_kind == ScalarFpKind::Classify) {
    emit_scalar_fclass_s(di, pc);
    return true;
  }

  throw EmitError("unsupported.inst", func_name, pc, di.name);
}

} // namespace sbt::ptx::detail
