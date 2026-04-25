#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

bool try_emit_custom(EmitCtx &ctx, const sbt::DecodedInst &di) {
  const uint32_t pc = di.pc;

  auto unpack_u32_to_halves = [&](const std::string &src_u32, const std::string &lo_h16, const std::string &hi_h16) {
    ctx.emit_line("mov.b32 {" + lo_h16 + ", " + hi_h16 + "}, " + src_u32 + ";");
  };
  auto pack_halves_to_u32 = [&](const std::string &dst_u32, const std::string &lo_h16, const std::string &hi_h16) {
    ctx.emit_line("mov.b32 " + dst_u32 + ", {" + lo_h16 + ", " + hi_h16 + "};");
  };
  auto emit_tanh_f32 = [&](const std::string &dst_f, const std::string &src_f) {
    const std::string tmp0 = ctx.tmp_f32();
    const std::string tmp1 = ctx.tmp_f32();
    ctx.emit_line("mul.rn.f32 " + tmp0 + ", " + src_f + ", -2.8853900817779268;");
    ctx.emit_line("ex2.approx.f32 " + tmp0 + ", " + tmp0 + ";");
    ctx.emit_line("add.rn.f32 " + tmp1 + ", " + tmp0 + ", 1.0;");
    ctx.emit_line("rcp.approx.f32 " + tmp1 + ", " + tmp1 + ";");
    ctx.emit_line("mul.rn.f32 " + tmp1 + ", " + tmp1 + ", 2.0;");
    ctx.emit_line("add.rn.f32 " + dst_f + ", " + tmp1 + ", -1.0;");
  };
  auto emit_silu_f32 = [&](const std::string &dst_f, const std::string &src_f) {
    const std::string tmp0 = ctx.tmp_f32();
    const std::string tmp1 = ctx.tmp_f32();
    ctx.emit_line("mul.rn.f32 " + tmp0 + ", " + src_f + ", -1.4426950408889634;");
    ctx.emit_line("ex2.approx.f32 " + tmp0 + ", " + tmp0 + ";");
    ctx.emit_line("add.rn.f32 " + tmp1 + ", " + tmp0 + ", 1.0;");
    ctx.emit_line("rcp.approx.f32 " + tmp1 + ", " + tmp1 + ";");
    ctx.emit_line("mul.rn.f32 " + dst_f + ", " + src_f + ", " + tmp1 + ";");
  };
  auto emit_gelu_f32 = [&](const std::string &dst_f, const std::string &src_f) {
    const std::string poly = ctx.tmp_f32();
    const std::string tanh_v = ctx.tmp_f32();
    ctx.emit_line("mul.rn.f32 " + poly + ", " + src_f + ", " + src_f + ";");
    ctx.emit_line("mul.rn.f32 " + poly + ", " + poly + ", " + src_f + ";");
    ctx.emit_line("mad.rn.f32 " + poly + ", " + poly + ", 0.044715, " + src_f + ";");
    ctx.emit_line("mul.rn.f32 " + poly + ", " + poly + ", 0.7978845608028654;");
    emit_tanh_f32(tanh_v, poly);
    ctx.emit_line("add.rn.f32 " + tanh_v + ", " + tanh_v + ", 1.0;");
    ctx.emit_line("mul.rn.f32 " + tanh_v + ", " + tanh_v + ", 0.5;");
    ctx.emit_line("mul.rn.f32 " + dst_f + ", " + src_f + ", " + tanh_v + ";");
  };
  auto emit_custom_sfu_f32 = [&](const std::string &dst_f, const std::string &src_f, sbt::CustomSubOp subop) {
    switch (subop) {
    case sbt::CustomSubOp::Ex2: ctx.emit_line("ex2.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Lg2: ctx.emit_line("lg2.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Rcp: ctx.emit_line("rcp.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Sqrt: ctx.emit_line("sqrt.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Rsqrt: ctx.emit_line("rsqrt.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Sin: ctx.emit_line("sin.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Cos: ctx.emit_line("cos.approx.f32 " + dst_f + ", " + src_f + ";"); return;
    case sbt::CustomSubOp::Tanh: emit_tanh_f32(dst_f, src_f); return;
    case sbt::CustomSubOp::Gelu: emit_gelu_f32(dst_f, src_f); return;
    case sbt::CustomSubOp::Silu: emit_silu_f32(dst_f, src_f); return;
    default: throw EmitError("unsupported.custom.sfu", ctx.func_name, pc, di.name);
    }
  };
  auto emit_custom_rsqrt_dual_lane = [&](bool bf16_kind, const std::string &src_h16, const std::string &src_f32,
                                         const std::string &dst_f32, const std::string &dst_h16) {
    const std::string exp_mask = bf16_kind ? "0x7f80" : "0x7c00";
    const std::string frac_mask = bf16_kind ? "0x007f" : "0x03ff";
    const std::string pos_inf = bf16_kind ? "0x7f80" : "0x7c00";
    const std::string convert_to_f32 = bf16_kind ? "cvt.f32.bf16 " : "cvt.f32.f16 ";
    const std::string convert_from_f32 = bf16_kind ? "cvt.rn.bf16.f32 " : "cvt.rn.f16.f32 ";
    const std::string src_u16 = ctx.tmp_u16();
    const std::string src_u32 = ctx.tmp_b32();
    const std::string sign_bits = ctx.tmp_b32();
    const std::string exp_bits = ctx.tmp_b32();
    const std::string frac_bits = ctx.tmp_b32();
    const std::string is_sign = ctx.tmp_pred();
    const std::string is_exp_zero = ctx.tmp_pred();
    const std::string is_exp_all1 = ctx.tmp_pred();
    const std::string is_frac_zero = ctx.tmp_pred();
    const std::string is_zero = ctx.tmp_pred();
    const std::string is_inf = ctx.tmp_pred();
    const std::string is_nan = ctx.tmp_pred();
    const std::string is_neg_nonzero = ctx.tmp_pred();
    const std::string is_pos_inf = ctx.tmp_pred();
    ctx.emit_line("mov.b16 " + src_u16 + ", " + src_h16 + ";");
    ctx.emit_line("cvt.u32.u16 " + src_u32 + ", " + src_u16 + ";");
    ctx.emit_line("and.b32 " + sign_bits + ", " + src_u32 + ", 0x8000;");
    ctx.emit_line("and.b32 " + exp_bits + ", " + src_u32 + ", " + exp_mask + ";");
    ctx.emit_line("and.b32 " + frac_bits + ", " + src_u32 + ", " + frac_mask + ";");
    ctx.emit_line("setp.ne.u32 " + is_sign + ", " + sign_bits + ", 0;");
    ctx.emit_line("setp.eq.u32 " + is_exp_zero + ", " + exp_bits + ", 0;");
    ctx.emit_line("setp.eq.u32 " + is_exp_all1 + ", " + exp_bits + ", " + exp_mask + ";");
    ctx.emit_line("setp.eq.u32 " + is_frac_zero + ", " + frac_bits + ", 0;");
    ctx.emit_line("and.pred " + is_zero + ", " + is_exp_zero + ", " + is_frac_zero + ";");
    ctx.emit_line("and.pred " + is_inf + ", " + is_exp_all1 + ", " + is_frac_zero + ";");
    ctx.emit_line("not.pred " + is_nan + ", " + is_frac_zero + ";");
    ctx.emit_line("and.pred " + is_nan + ", " + is_exp_all1 + ", " + is_nan + ";");
    ctx.emit_line("not.pred " + is_neg_nonzero + ", " + is_zero + ";");
    ctx.emit_line("and.pred " + is_neg_nonzero + ", " + is_sign + ", " + is_neg_nonzero + ";");
    ctx.emit_line("not.pred " + is_pos_inf + ", " + is_sign + ";");
    ctx.emit_line("and.pred " + is_pos_inf + ", " + is_inf + ", " + is_pos_inf + ";");
    ctx.emit_line(convert_to_f32 + src_f32 + ", " + src_h16 + ";");
    emit_custom_sfu_f32(dst_f32, src_f32, sbt::CustomSubOp::Rsqrt);
    ctx.emit_line(convert_from_f32 + dst_h16 + ", " + dst_f32 + ";");
    ctx.emit_line("@" + is_zero + " mov.b16 " + dst_h16 + ", " + pos_inf + ";");
    ctx.emit_line("@" + is_neg_nonzero + " mov.b16 " + dst_h16 + ", 0x7fff;");
    ctx.emit_line("@" + is_pos_inf + " mov.b16 " + dst_h16 + ", 0;");
    ctx.emit_line("@" + is_nan + " mov.b16 " + dst_h16 + ", 0x7fff;");
  };

  if (!(di.emit.domain == EmitDomain::Custom && di.custom.valid)) return false;

  if (di.custom.family == sbt::CustomFamily::Shuffle) {
    require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", ctx.func_name, pc, di.name));
    ctx.emit_read_activemask(r(1));
    const std::string shuffle_arg = ctx.tmp_b32();
    ctx.emit_line("mov.u32 " + shuffle_arg + ", " + std::to_string(di.imm & 31) + ";");
    if (di.custom.subop == sbt::CustomSubOp::ShuffleIdx) {
      ctx.emit_line("shfl.sync.idx.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x1f, " + r(1) + ";");
    } else if (di.custom.subop == sbt::CustomSubOp::ShuffleUp) {
      ctx.emit_line("shfl.sync.up.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x0, " + r(1) + ";");
    } else if (di.custom.subop == sbt::CustomSubOp::ShuffleDown) {
      ctx.emit_line("shfl.sync.down.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x1f, " + r(1) + ";");
    } else if (di.custom.subop == sbt::CustomSubOp::ShuffleBfly) {
      ctx.emit_line("shfl.sync.bfly.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x1f, " + r(1) + ";");
    } else {
      throw EmitError("invalid.custom.payload", ctx.func_name, pc, di.name);
    }
    return true;
  }

  if (di.custom.family == sbt::CustomFamily::Convert &&
      di.custom.subop == sbt::CustomSubOp::CvtF32FromF16 && di.custom.dtype == sbt::CustomDataType::Fp16) {
    const std::string lo_half = ctx.tmp_b16();
    const std::string hi_half = ctx.tmp_b16();
    const std::string dst = ctx.tmp_f32();
    unpack_u32_to_halves(v(di.rs2), lo_half, hi_half);
    ctx.emit_line("cvt.f32.f16 " + dst + ", " + lo_half + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + dst + ";");
    return true;
  }
  if (di.custom.family == sbt::CustomFamily::Convert &&
      di.custom.subop == sbt::CustomSubOp::CvtF16FromF32 && di.custom.dtype == sbt::CustomDataType::Fp16) {
    const std::string src = ctx.tmp_f32();
    const std::string lo_half = ctx.tmp_b16();
    const std::string hi_half = ctx.tmp_b16();
    const std::string packed = ctx.tmp_b32();
    ctx.emit_line("mov.b32 " + src + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.rn.f16.f32 " + lo_half + ", " + src + ";");
    ctx.emit_line("mov.b16 " + hi_half + ", 0;");
    pack_halves_to_u32(packed, lo_half, hi_half);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
    return true;
  }
  if (di.custom.family == sbt::CustomFamily::Convert &&
      di.custom.subop == sbt::CustomSubOp::CvtF32FromBf16 && di.custom.dtype == sbt::CustomDataType::Bf16) {
    const std::string lo_half = ctx.tmp_b16();
    const std::string hi_half = ctx.tmp_b16();
    const std::string dst = ctx.tmp_f32();
    unpack_u32_to_halves(v(di.rs2), lo_half, hi_half);
    ctx.emit_line("cvt.f32.bf16 " + dst + ", " + lo_half + ";");
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + dst + ";");
    return true;
  }
  if (di.custom.family == sbt::CustomFamily::Convert &&
      di.custom.subop == sbt::CustomSubOp::CvtBf16FromF32 && di.custom.dtype == sbt::CustomDataType::Bf16) {
    const std::string src = ctx.tmp_f32();
    const std::string lo_half = ctx.tmp_b16();
    const std::string hi_half = ctx.tmp_b16();
    const std::string packed = ctx.tmp_b32();
    ctx.emit_line("mov.b32 " + src + ", " + v(di.rs2) + ";");
    ctx.emit_line("cvt.rn.bf16.f32 " + lo_half + ", " + src + ";");
    ctx.emit_line("mov.b16 " + hi_half + ", 0;");
    pack_halves_to_u32(packed, lo_half, hi_half);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
    return true;
  }

  if (di.custom.family == sbt::CustomFamily::PackedArith && di.custom.dtype == sbt::CustomDataType::F16x2) {
    const std::string packed = ctx.tmp_b32();
    if (di.custom.subop == sbt::CustomSubOp::Add) ctx.emit_line("add.rn.f16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
    else if (di.custom.subop == sbt::CustomSubOp::Mul) ctx.emit_line("mul.rn.f16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
    else if (di.custom.subop == sbt::CustomSubOp::Fma) ctx.emit_line("fma.rn.f16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
    else throw EmitError("invalid.custom.payload", ctx.func_name, pc, di.name);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
    return true;
  }

  if (di.custom.family == sbt::CustomFamily::PackedArith && di.custom.dtype == sbt::CustomDataType::Bf16x2) {
    if (di.custom.subop == sbt::CustomSubOp::Fma) {
      const std::string packed = ctx.tmp_b32();
      ctx.emit_line("fma.rn.bf16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return true;
    }
    const std::string a0 = ctx.tmp_b16();
    const std::string a1 = ctx.tmp_b16();
    const std::string b0 = ctx.tmp_b16();
    const std::string b1 = ctx.tmp_b16();
    const std::string fa0 = ctx.tmp_f32();
    const std::string fa1 = ctx.tmp_f32();
    const std::string fb0 = ctx.tmp_f32();
    const std::string fb1 = ctx.tmp_f32();
    const std::string out0 = ctx.tmp_f32();
    const std::string out1 = ctx.tmp_f32();
    const std::string packed0 = ctx.tmp_b16();
    const std::string packed1 = ctx.tmp_b16();
    const std::string packed = ctx.tmp_b32();
    unpack_u32_to_halves(v(di.rs1), a0, a1);
    unpack_u32_to_halves(v(di.rs2), b0, b1);
    ctx.emit_line("cvt.f32.bf16 " + fa0 + ", " + a0 + ";");
    ctx.emit_line("cvt.f32.bf16 " + fa1 + ", " + a1 + ";");
    ctx.emit_line("cvt.f32.bf16 " + fb0 + ", " + b0 + ";");
    ctx.emit_line("cvt.f32.bf16 " + fb1 + ", " + b1 + ";");
    if (di.custom.subop == sbt::CustomSubOp::Add) {
      ctx.emit_line("add.rn.f32 " + out0 + ", " + fa0 + ", " + fb0 + ";");
      ctx.emit_line("add.rn.f32 " + out1 + ", " + fa1 + ", " + fb1 + ";");
    } else if (di.custom.subop == sbt::CustomSubOp::Mul) {
      ctx.emit_line("mul.rn.f32 " + out0 + ", " + fa0 + ", " + fb0 + ";");
      ctx.emit_line("mul.rn.f32 " + out1 + ", " + fa1 + ", " + fb1 + ";");
    } else {
      throw EmitError("invalid.custom.payload", ctx.func_name, pc, di.name);
    }
    ctx.emit_line("cvt.rn.bf16.f32 " + packed0 + ", " + out0 + ";");
    ctx.emit_line("cvt.rn.bf16.f32 " + packed1 + ", " + out1 + ";");
    pack_halves_to_u32(packed, packed0, packed1);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
    return true;
  }

  if (di.custom.family == sbt::CustomFamily::Sfu && di.custom.dtype == sbt::CustomDataType::Fp32) {
    require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", ctx.func_name, pc, di.name));
    const std::string src = ctx.tmp_f32();
    const std::string dst = ctx.tmp_f32();
    ctx.emit_line("mov.b32 " + src + ", " + v(di.rs2) + ";");
    emit_custom_sfu_f32(dst, src, di.custom.subop);
    ctx.emit_line("mov.b32 " + v(di.rd) + ", " + dst + ";");
    return true;
  }

  if (di.custom.family == sbt::CustomFamily::Sfu && di.custom.dtype == sbt::CustomDataType::F16x2) {
    require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", ctx.func_name, pc, di.name));
    const std::string in0 = ctx.tmp_b16();
    const std::string in1 = ctx.tmp_b16();
    const std::string out0 = ctx.tmp_b16();
    const std::string out1 = ctx.tmp_b16();
    const std::string f0v = ctx.tmp_f32();
    const std::string f1v = ctx.tmp_f32();
    const std::string f2v = ctx.tmp_f32();
    const std::string f3v = ctx.tmp_f32();
    const std::string packed = ctx.tmp_b32();
    unpack_u32_to_halves(v(di.rs2), in0, in1);
    if (di.custom.subop == sbt::CustomSubOp::Rsqrt) {
      emit_custom_rsqrt_dual_lane(/*bf16_kind=*/false, in0, f0v, f2v, out0);
      emit_custom_rsqrt_dual_lane(/*bf16_kind=*/false, in1, f1v, f3v, out1);
    } else {
      ctx.emit_line("cvt.f32.f16 " + f0v + ", " + in0 + ";");
      ctx.emit_line("cvt.f32.f16 " + f1v + ", " + in1 + ";");
      emit_custom_sfu_f32(f2v, f0v, di.custom.subop);
      emit_custom_sfu_f32(f3v, f1v, di.custom.subop);
      ctx.emit_line("cvt.rn.f16.f32 " + out0 + ", " + f2v + ";");
      ctx.emit_line("cvt.rn.f16.f32 " + out1 + ", " + f3v + ";");
    }
    pack_halves_to_u32(packed, out0, out1);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
    return true;
  }

  if (di.custom.family == sbt::CustomFamily::Sfu && di.custom.dtype == sbt::CustomDataType::Bf16x2) {
    require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", ctx.func_name, pc, di.name));
    const std::string in0 = ctx.tmp_b16();
    const std::string in1 = ctx.tmp_b16();
    const std::string out0 = ctx.tmp_b16();
    const std::string out1 = ctx.tmp_b16();
    const std::string f0v = ctx.tmp_f32();
    const std::string f1v = ctx.tmp_f32();
    const std::string f2v = ctx.tmp_f32();
    const std::string f3v = ctx.tmp_f32();
    const std::string packed = ctx.tmp_b32();
    unpack_u32_to_halves(v(di.rs2), in0, in1);
    if (di.custom.subop == sbt::CustomSubOp::Rsqrt) {
      emit_custom_rsqrt_dual_lane(/*bf16_kind=*/true, in0, f0v, f2v, out0);
      emit_custom_rsqrt_dual_lane(/*bf16_kind=*/true, in1, f1v, f3v, out1);
    } else {
      ctx.emit_line("cvt.f32.bf16 " + f0v + ", " + in0 + ";");
      ctx.emit_line("cvt.f32.bf16 " + f1v + ", " + in1 + ";");
      emit_custom_sfu_f32(f2v, f0v, di.custom.subop);
      emit_custom_sfu_f32(f3v, f1v, di.custom.subop);
      ctx.emit_line("cvt.rn.bf16.f32 " + out0 + ", " + f2v + ";");
      ctx.emit_line("cvt.rn.bf16.f32 " + out1 + ", " + f3v + ";");
    }
    pack_halves_to_u32(packed, out0, out1);
    ctx.emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
    return true;
  }

  return false;
}

} // namespace sbt::ptx::detail
