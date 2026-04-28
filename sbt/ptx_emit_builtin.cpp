#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

static void emit_builtin_global_dim(EmitCtx &ctx, const char *tid, const char *ntid, const char *ctaid) {
  const std::string tid_r = ctx.tmp_b32();
  const std::string ntid_r = ctx.tmp_b32();
  const std::string ctaid_r = ctx.tmp_b32();
  const std::string gid = ctx.tmp_b32();
  ctx.emit_line("mov.u32 " + tid_r + ", " + std::string(tid) + ";");
  ctx.emit_line("mov.u32 " + ntid_r + ", " + std::string(ntid) + ";");
  ctx.emit_line("mov.u32 " + ctaid_r + ", " + std::string(ctaid) + ";");
  ctx.emit_line("mul.lo.u32 " + gid + ", " + ctaid_r + ", " + ntid_r + ";");
  ctx.emit_line("add.u32 " + gid + ", " + gid + ", " + tid_r + ";");
  ctx.emit_line("mov.u32 " + v(0) + ", " + gid + ";");
}

void emit_builtin_call(EmitCtx &ctx, BuiltinKind kind, uint32_t pc_for_err) {
  switch (kind) {
  case BuiltinKind::GetGlobalId: ctx.emit_builtin_get_id("global", pc_for_err); return;
  case BuiltinKind::GetLocalId: ctx.emit_builtin_get_id("local", pc_for_err); return;
  case BuiltinKind::GetGroupId: ctx.emit_builtin_get_id("group", pc_for_err); return;
  case BuiltinKind::GetGlobalSize: ctx.emit_builtin_get_global_size(pc_for_err); return;
  case BuiltinKind::WorkitemIdX: ctx.emit_line("mov.u32 " + v(0) + ", %tid.x;"); return;
  case BuiltinKind::WorkitemIdY: ctx.emit_line("mov.u32 " + v(0) + ", %tid.y;"); return;
  case BuiltinKind::WorkitemIdZ: ctx.emit_line("mov.u32 " + v(0) + ", %tid.z;"); return;
  case BuiltinKind::WorkgroupIdX: ctx.emit_line("mov.u32 " + v(0) + ", %ctaid.x;"); return;
  case BuiltinKind::WorkgroupIdY: ctx.emit_line("mov.u32 " + v(0) + ", %ctaid.y;"); return;
  case BuiltinKind::WorkgroupIdZ: ctx.emit_line("mov.u32 " + v(0) + ", %ctaid.z;"); return;
  case BuiltinKind::GlobalIdX: emit_builtin_global_dim(ctx, "%tid.x", "%ntid.x", "%ctaid.x"); return;
  case BuiltinKind::GlobalIdY: emit_builtin_global_dim(ctx, "%tid.y", "%ntid.y", "%ctaid.y"); return;
  case BuiltinKind::GlobalIdZ: emit_builtin_global_dim(ctx, "%tid.z", "%ntid.z", "%ctaid.z"); return;
  case BuiltinKind::SqrtF: ctx.emit_builtin_sqrtf(pc_for_err); return;
  case BuiltinKind::FmaxF: ctx.emit_builtin_fmaxff(pc_for_err); return;
  case BuiltinKind::Vec4Cos: ctx.emit_builtin_vec4_cos(pc_for_err); return;
  case BuiltinKind::Vec4Sin: ctx.emit_builtin_vec4_sin(pc_for_err); return;
  case BuiltinKind::Vec4Tan: ctx.emit_builtin_vec4_tan(pc_for_err); return;
  case BuiltinKind::Vec4Sqrt: ctx.emit_builtin_vec4_sqrt(pc_for_err); return;
  case BuiltinKind::Vec4Fabs: ctx.emit_builtin_vec4_fabs(pc_for_err); return;
  case BuiltinKind::Mad24: ctx.emit_builtin_mad24iii(pc_for_err); return;
  }
  throw EmitError("unsupported.call", ctx.func_name, pc_for_err, "builtin dispatch");
}

void EmitCtx::emit_builtin_get_id(std::string_view kind, uint32_t pc_for_err) {
  // Uses %v0 as "dim" input for get_*_idj; writes result to %v0.
  // kind: "global"|"local"|"group"
  const std::string dim = tmp_b32();
  emit_line("// builtin: " + std::string(kind) + "_id(dim=v0) -> v0");
  emit_line("mov.u32 " + dim + ", " + v(0) + ";"); // dim

  const std::string L_dim0 = new_label("dim0");
  const std::string L_dim1 = new_label("dim1");
  const std::string L_dim2 = new_label("dim2");
  const std::string L_done = new_label("dim_done");

  const std::string is_dim0 = tmp_pred();
  const std::string is_dim1 = tmp_pred();
  const std::string is_dim2 = tmp_pred();
  emit_line("setp.eq.u32 " + is_dim0 + ", " + dim + ", 0;");
  emit_line("@" + is_dim0 + " bra " + L_dim0 + ";");
  emit_line("setp.eq.u32 " + is_dim1 + ", " + dim + ", 1;");
  emit_line("@" + is_dim1 + " bra " + L_dim1 + ";");
  emit_line("setp.eq.u32 " + is_dim2 + ", " + dim + ", 2;");
  emit_line("@" + is_dim2 + " bra " + L_dim2 + ";");

  emit_line("mov.u32 " + v(0) + ", 0;");
  emit_line("bra " + L_done + ";");

  auto emit_dim = [&](const std::string &L, const char *tid, const char *ntid, const char *ctaid) {
    emit_label(L);
    if (kind == "local") {
      emit_line("mov.u32 " + v(0) + ", " + std::string(tid) + ";");
    } else if (kind == "group") {
      emit_line("mov.u32 " + v(0) + ", " + std::string(ctaid) + ";");
    } else {
      // global
      const std::string tid_r = tmp_b32();
      const std::string ntid_r = tmp_b32();
      const std::string ctaid_r = tmp_b32();
      const std::string gid_r = tmp_b32();
      emit_line("mov.u32 " + tid_r + ", " + std::string(tid) + ";");
      emit_line("mov.u32 " + ntid_r + ", " + std::string(ntid) + ";");
      emit_line("mov.u32 " + ctaid_r + ", " + std::string(ctaid) + ";");
      emit_line("mul.lo.u32 " + gid_r + ", " + ctaid_r + ", " + ntid_r + ";");
      emit_line("add.u32 " + gid_r + ", " + gid_r + ", " + tid_r + ";");
      emit_line("mov.u32 " + v(0) + ", " + gid_r + ";");
    }
    emit_line("bra " + L_done + ";");
  };

  emit_dim(L_dim0, "%tid.x", "%ntid.x", "%ctaid.x");
  emit_dim(L_dim1, "%tid.y", "%ntid.y", "%ctaid.y");
  emit_dim(L_dim2, "%tid.z", "%ntid.z", "%ctaid.z");

  emit_label(L_done);
  (void)pc_for_err;
}

void EmitCtx::emit_builtin_get_global_size(uint32_t pc_for_err) {
  // Uses %v0 as "dim" input for get_global_sizej; writes result to %v0.
  // Returns the launched global size, i.e. gridDim * blockDim per dimension.
  const std::string dim = tmp_b32();
  emit_line("// builtin: global_size(dim=v0) -> v0");
  emit_line("mov.u32 " + dim + ", " + v(0) + ";"); // dim

  const std::string L_dim0 = new_label("gsize_dim0");
  const std::string L_dim1 = new_label("gsize_dim1");
  const std::string L_dim2 = new_label("gsize_dim2");
  const std::string L_done = new_label("gsize_done");

  const std::string is_dim0 = tmp_pred();
  const std::string is_dim1 = tmp_pred();
  const std::string is_dim2 = tmp_pred();
  emit_line("setp.eq.u32 " + is_dim0 + ", " + dim + ", 0;");
  emit_line("@" + is_dim0 + " bra " + L_dim0 + ";");
  emit_line("setp.eq.u32 " + is_dim1 + ", " + dim + ", 1;");
  emit_line("@" + is_dim1 + " bra " + L_dim1 + ";");
  emit_line("setp.eq.u32 " + is_dim2 + ", " + dim + ", 2;");
  emit_line("@" + is_dim2 + " bra " + L_dim2 + ";");

  // For out-of-range dims, return 1 (matches typical OpenCL behavior for unused dims).
  emit_line("mov.u32 " + v(0) + ", 1;");
  emit_line("bra " + L_done + ";");

  auto emit_dim = [&](const std::string &L, const char *ntid, const char *nctaid) {
    const std::string ntid_r = tmp_b32();
    const std::string nctaid_r = tmp_b32();
    const std::string size_r = tmp_b32();
    emit_label(L);
    emit_line("mov.u32 " + ntid_r + ", " + std::string(ntid) + ";");
    emit_line("mov.u32 " + nctaid_r + ", " + std::string(nctaid) + ";");
    emit_line("mul.lo.u32 " + size_r + ", " + ntid_r + ", " + nctaid_r + ";");
    emit_line("mov.u32 " + v(0) + ", " + size_r + ";");
    emit_line("bra " + L_done + ";");
  };

  emit_dim(L_dim0, "%ntid.x", "%nctaid.x");
  emit_dim(L_dim1, "%ntid.y", "%nctaid.y");
  emit_dim(L_dim2, "%ntid.z", "%nctaid.z");

  emit_label(L_done);
  (void)pc_for_err;
}

void EmitCtx::emit_builtin_fmaxff(uint32_t pc_for_err) {
  // OpenCL/C: float fmax(float a, float b)
  // Calling convention (ventus clc): a=v0, b=v1, ret=v0 (per-thread scalar).
  // Semantics: return the numeric maximum; if exactly one is NaN, return the other; if both NaN, return NaN.
  const std::string lhs = tmp_f32();
  const std::string rhs = tmp_f32();
  const std::string result = tmp_f32();
  const std::string lhs_nan = tmp_pred();
  const std::string rhs_nan = tmp_pred();
  emit_line("// builtin: fmax(v0,v1) -> v0 (f32 bits, NaN-safe)");
  emit_line("mov.b32 " + lhs + ", " + v(0) + ";");
  emit_line("mov.b32 " + rhs + ", " + v(1) + ";");
  emit_line("setp.nan.f32 " + lhs_nan + ", " + lhs + ", " + lhs + ";");
  emit_line("setp.nan.f32 " + rhs_nan + ", " + rhs + ", " + rhs + ";");
  emit_line("max.f32 " + result + ", " + lhs + ", " + rhs + ";");
  emit_line("selp.b32 " + result + ", " + rhs + ", " + result + ", " + lhs_nan + ";");
  emit_line("selp.b32 " + result + ", " + lhs + ", " + result + ", " + rhs_nan + ";");
  emit_line("mov.b32 " + v(0) + ", " + result + ";");
  (void)pc_for_err;
}

void EmitCtx::emit_builtin_sqrtf(uint32_t pc_for_err) {
  const std::string src = tmp_f32();
  const std::string dst = tmp_f32();
  emit_line("// builtin: sqrtf(v0) -> v0 (f32 bits)");
  emit_line("mov.b32 " + src + ", " + v(0) + ";");
  emit_line("sqrt.rn.f32 " + dst + ", " + src + ";");
  emit_line("mov.b32 " + v(0) + ", " + dst + ";");
  (void)pc_for_err;
}

void EmitCtx::emit_builtin_unary_f32_inplace(std::string_view opname, const std::string &dst_v, uint32_t pc_for_err) {
  const std::string src = tmp_f32();
  const std::string dst = tmp_f32();
  emit_line("mov.b32 " + src + ", " + dst_v + ";");
  if (opname == "sqrt") {
    emit_line("sqrt.rn.f32 " + dst + ", " + src + ";");
  } else if (opname == "cos") {
    emit_line("cos.approx.f32 " + dst + ", " + src + ";");
  } else if (opname == "sin") {
    emit_line("sin.approx.f32 " + dst + ", " + src + ";");
  } else {
    throw EmitError("unsupported.call", func_name, pc_for_err, "unary_op=" + std::string(opname));
  }
  emit_line("mov.b32 " + dst_v + ", " + dst + ";");
}

void EmitCtx::emit_builtin_vec4_cos(uint32_t pc_for_err) {
  emit_line("// builtin: cos(float4) in v0..v3");
  for (int i = 0; i < 4; ++i) emit_builtin_unary_f32_inplace("cos", v(i), pc_for_err);
}

void EmitCtx::emit_builtin_vec4_sin(uint32_t pc_for_err) {
  emit_line("// builtin: sin(float4) in v0..v3");
  for (int i = 0; i < 4; ++i) emit_builtin_unary_f32_inplace("sin", v(i), pc_for_err);
}

void EmitCtx::emit_builtin_vec4_sqrt(uint32_t pc_for_err) {
  emit_line("// builtin: sqrt(float4) in v0..v3");
  for (int i = 0; i < 4; ++i) emit_builtin_unary_f32_inplace("sqrt", v(i), pc_for_err);
}

void EmitCtx::emit_builtin_vec4_fabs(uint32_t pc_for_err) {
  emit_line("// builtin: fabs(float4) in v0..v3 (bitwise clear sign)");
  for (int i = 0; i < 4; ++i) emit_line("and.b32 " + v(i) + ", " + v(i) + ", 0x7fffffff;");
  (void)pc_for_err;
}

void EmitCtx::emit_builtin_vec4_tan(uint32_t pc_for_err) {
  emit_line("// builtin: tan(float4) in v0..v3 (approx via sin/cos)");
  for (int i = 0; i < 4; ++i) {
    const std::string src = tmp_f32();
    const std::string sin_v = tmp_f32();
    const std::string cos_v = tmp_f32();
    const std::string tan_v = tmp_f32();
    emit_line("mov.b32 " + src + ", " + v(i) + ";");
    emit_line("sin.approx.f32 " + sin_v + ", " + src + ";");
    emit_line("cos.approx.f32 " + cos_v + ", " + src + ";");
    emit_line("div.rn.f32 " + tan_v + ", " + sin_v + ", " + cos_v + ";");
    emit_line("mov.b32 " + v(i) + ", " + tan_v + ";");
  }
  (void)pc_for_err;
}

void EmitCtx::emit_builtin_mad24iii(uint32_t pc_for_err) {
  // OpenCL: int mad24(int a, int b, int c) => mul24(a,b) + c (signed 24-bit multiply).
  // Calling convention (ventus clc): a=v0, b=v1, c=v2, ret=v0.
  const std::string lhs = tmp_b32();
  const std::string rhs = tmp_b32();
  const std::string acc = tmp_b32();
  emit_line("// builtin: mad24(v0,v1,v2) -> v0 (signed 24-bit)");
  emit_line("mov.b32 " + lhs + ", " + v(0) + ";");
  emit_line("mov.b32 " + rhs + ", " + v(1) + ";");
  emit_line("mov.b32 " + acc + ", " + v(2) + ";");
  // sign-extend low 24 bits: (x << 8) >> 8
  emit_line("shl.b32 " + lhs + ", " + lhs + ", 8;");
  emit_line("shr.s32 " + lhs + ", " + lhs + ", 8;");
  emit_line("shl.b32 " + rhs + ", " + rhs + ", 8;");
  emit_line("shr.s32 " + rhs + ", " + rhs + ", 8;");
  emit_line("mad.lo.s32 " + lhs + ", " + lhs + ", " + rhs + ", " + acc + ";");
  emit_line("mov.b32 " + v(0) + ", " + lhs + ";");
  (void)pc_for_err;
}

} // namespace sbt::ptx::detail

namespace sbt::ptx {

bool is_inlined_builtin_call_name(std::string_view callee) {
  return sbt::is_inlined_builtin_call_name(callee);
}

} // namespace sbt::ptx
