// OpenCL micro-tests for non-MMA custom instructions.
//
// Convention:
//   __kernel void K(__global const uint *A, __global uint *B)
// with A and B each containing N uint32 elements.
//
// These kernels intentionally use the upstream Ventus LLVM builtins so the
// compiler emits the same opcode/funct encodings as Spike/RTL.
//
// Validation kernels stay single-op and branch-free on purpose. This avoids
// mixing SIMT control-flow effects with the custom instruction under test,
// especially for cross-lane shuffle instructions.

typedef unsigned int uint;

static inline uint shuffle_idx_lane7(uint x) { return __builtin_riscv_ventus_shuffle_idx_i32(x, 7); }
static inline uint shuffle_up_lane1(uint x) { return __builtin_riscv_ventus_shuffle_up_i32(x, 1); }
static inline uint shuffle_down_lane2(uint x) { return __builtin_riscv_ventus_shuffle_down_i32(x, 2); }
static inline uint shuffle_bfly_lane8(uint x) { return __builtin_riscv_ventus_shuffle_bfly_i32(x, 8); }

static inline float vcvt_fp32_fp16(uint x) { return __builtin_riscv_ventus_vcvt_fp32_fp16(x); }
static inline uint vcvt_fp16_fp32(float x) { return __builtin_riscv_ventus_vcvt_fp16_fp32(x); }
static inline float vcvt_fp32_bf16(uint x) { return __builtin_riscv_ventus_vcvt_fp32_bf16(x); }
static inline uint vcvt_bf16_fp32(float x) { return __builtin_riscv_ventus_vcvt_bf16_fp32(x); }

static inline uint vadd_f16x2(uint a, uint b) { return __builtin_riscv_ventus_vadd_f16x2(a, b); }
static inline uint vmul_f16x2(uint a, uint b) { return __builtin_riscv_ventus_vmul_f16x2(a, b); }
static inline uint vfma_f16x2(uint a, uint b, uint c) { return __builtin_riscv_ventus_vfma_f16x2(a, b, c); }
static inline uint vadd_bf16x2(uint a, uint b) { return __builtin_riscv_ventus_vadd_bf16x2(a, b); }
static inline uint vmul_bf16x2(uint a, uint b) { return __builtin_riscv_ventus_vmul_bf16x2(a, b); }
static inline uint vfma_bf16x2(uint a, uint b, uint c) { return __builtin_riscv_ventus_vfma_bf16x2(a, b, c); }

static inline float vex2_approx_f32(float x) { return __builtin_riscv_ventus_vex2_approx_f32(x); }
static inline float vlg2_approx_f32(float x) { return __builtin_riscv_ventus_vlg2_approx_f32(x); }
static inline float vrcp_approx_f32(float x) { return __builtin_riscv_ventus_vrcp_approx_f32(x); }
static inline float vsqrt_approx_f32(float x) { return __builtin_riscv_ventus_vsqrt_approx_f32(x); }
static inline float vrsqrt_approx_f32(float x) { return __builtin_riscv_ventus_vrsqrt_approx_f32(x); }
static inline float vsin_approx_f32(float x) { return __builtin_riscv_ventus_vsin_approx_f32(x); }
static inline float vcos_approx_f32(float x) { return __builtin_riscv_ventus_vcos_approx_f32(x); }
static inline float vtanh_approx_f32(float x) { return __builtin_riscv_ventus_vtanh_approx_f32(x); }
static inline float vgelu_approx_f32(float x) { return __builtin_riscv_ventus_vgelu_approx_f32(x); }
static inline float vsilu_approx_f32(float x) { return __builtin_riscv_ventus_vsilu_approx_f32(x); }

static inline uint vex2_approx_f16x2(uint x) { return __builtin_riscv_ventus_vex2_approx_f16x2(x); }
static inline uint vrcp_approx_f16x2(uint x) { return __builtin_riscv_ventus_vrcp_approx_f16x2(x); }
static inline uint vsqrt_approx_f16x2(uint x) { return __builtin_riscv_ventus_vsqrt_approx_f16x2(x); }
static inline uint vrsqrt_approx_f16x2(uint x) { return __builtin_riscv_ventus_vrsqrt_approx_f16x2(x); }
static inline uint vtanh_approx_f16x2(uint x) { return __builtin_riscv_ventus_vtanh_approx_f16x2(x); }
static inline uint vgelu_approx_f16x2(uint x) { return __builtin_riscv_ventus_vgelu_approx_f16x2(x); }
static inline uint vsilu_approx_f16x2(uint x) { return __builtin_riscv_ventus_vsilu_approx_f16x2(x); }

static inline uint vex2_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vex2_approx_bf16x2(x); }
static inline uint vrcp_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vrcp_approx_bf16x2(x); }
static inline uint vsqrt_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vsqrt_approx_bf16x2(x); }
static inline uint vrsqrt_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vrsqrt_approx_bf16x2(x); }
static inline uint vtanh_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vtanh_approx_bf16x2(x); }
static inline uint vgelu_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vgelu_approx_bf16x2(x); }
static inline uint vsilu_approx_bf16x2(uint x) { return __builtin_riscv_ventus_vsilu_approx_bf16x2(x); }

static inline float signed_sample(uint gid, uint salt, float scale) {
  const int s = (int)(((gid * 37u) ^ salt) & 0x7fu) - 64;
  return ((float)s) * scale;
}

static inline float positive_sample(uint gid, uint salt, float scale, float bias) {
  return fabs(signed_sample(gid, salt, scale)) + bias;
}

static inline uint pack_f16x2(float lo, float hi) {
  const uint lo_bits = vcvt_fp16_fp32(lo);
  const uint hi_bits = vcvt_fp16_fp32(hi);
  return ((hi_bits & 0xffffu) << 16) | (lo_bits & 0xffffu);
}

static inline uint pack_bf16x2(float lo, float hi) {
  const uint lo_bits = vcvt_bf16_fp32(lo);
  const uint hi_bits = vcvt_bf16_fp32(hi);
  return ((hi_bits & 0xffffu) << 16) | (lo_bits & 0xffffu);
}

static inline uint sample_pack_f16x2_a(uint gid) {
  return pack_f16x2(signed_sample(gid, 0x11u, 0.03125f), signed_sample(gid, 0x22u, 0.015625f));
}

static inline uint sample_pack_f16x2_b(uint gid) {
  return pack_f16x2(signed_sample(gid, 0x33u, 0.03125f), signed_sample(gid, 0x44u, 0.015625f));
}

static inline uint sample_pack_f16x2_c(uint gid) {
  return pack_f16x2(signed_sample(gid, 0x55u, 0.03125f), signed_sample(gid, 0x66u, 0.015625f));
}

static inline uint sample_pack_f16x2_pos_a(uint gid) {
  return pack_f16x2(positive_sample(gid, 0x10u, 0.03125f, 0.25f), positive_sample(gid, 0x20u, 0.015625f, 0.25f));
}

static inline uint sample_pack_f16x2_pos_b(uint gid) {
  return pack_f16x2(positive_sample(gid, 0x30u, 0.03125f, 0.25f), positive_sample(gid, 0x40u, 0.015625f, 0.25f));
}

static inline uint sample_pack_bf16x2_a(uint gid) {
  return pack_bf16x2(signed_sample(gid, 0x77u, 0.03125f), signed_sample(gid, 0x88u, 0.015625f));
}

static inline uint sample_pack_bf16x2_b(uint gid) {
  return pack_bf16x2(signed_sample(gid, 0x99u, 0.03125f), signed_sample(gid, 0xaau, 0.015625f));
}

static inline uint sample_pack_bf16x2_c(uint gid) {
  return pack_bf16x2(signed_sample(gid, 0xbbu, 0.03125f), signed_sample(gid, 0xccu, 0.015625f));
}

static inline uint sample_pack_bf16x2_pos_a(uint gid) {
  return pack_bf16x2(positive_sample(gid, 0x50u, 0.03125f, 0.25f), positive_sample(gid, 0x60u, 0.015625f, 0.25f));
}

static inline uint sample_pack_bf16x2_pos_b(uint gid) {
  return pack_bf16x2(positive_sample(gid, 0x70u, 0.03125f, 0.25f), positive_sample(gid, 0x80u, 0.015625f, 0.25f));
}

#define DEFINE_SHUFFLE_KERNEL(NAME, OP)                                                                  \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    (void)A;                                                                                            \
    const uint gid = (uint)get_global_id(0);                                                            \
    B[gid] = OP(gid);                                                                                   \
  }

#define DEFINE_VCVT_ROUNDTRIP_KERNEL(NAME, PACK_OP, UNPACK_OP, SCALE, SALT)                             \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    (void)A;                                                                                            \
    const uint gid = (uint)get_global_id(0);                                                            \
    const float x = signed_sample(gid, SALT, SCALE);                                                    \
    B[gid] = as_uint(UNPACK_OP(PACK_OP(x)));                                                            \
  }

#define DEFINE_PACKED_BINARY_KERNEL(NAME, OP, PACK_A, PACK_B)                                            \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    (void)A;                                                                                            \
    const uint gid = (uint)get_global_id(0);                                                            \
    B[gid] = OP(PACK_A(gid), PACK_B(gid));                                                              \
  }

#define DEFINE_PACKED_TERNARY_KERNEL(NAME, OP, PACK_A, PACK_B, PACK_C)                                   \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    (void)A;                                                                                            \
    const uint gid = (uint)get_global_id(0);                                                            \
    B[gid] = OP(PACK_A(gid), PACK_B(gid), PACK_C(gid));                                                 \
  }

#define DEFINE_SFU_FP32_KERNEL(NAME, OP, X_EXPR)                                                         \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    (void)A;                                                                                            \
    const uint gid = (uint)get_global_id(0);                                                            \
    const float x = (X_EXPR);                                                                           \
    B[gid] = as_uint(OP(x));                                                                            \
  }

#define DEFINE_SFU_PACKED_KERNEL(NAME, OP, PACK_EXPR)                                                    \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    (void)A;                                                                                            \
    const uint gid = (uint)get_global_id(0);                                                            \
    B[gid] = OP(PACK_EXPR(gid));                                                                        \
  }

#define DEFINE_SFU_PACKED_INPUT_KERNEL(NAME, OP)                                                         \
  __kernel void NAME(__global const uint *A, __global uint *B) {                                        \
    const uint gid = (uint)get_global_id(0);                                                            \
    B[gid] = OP(A[gid]);                                                                                \
  }

DEFINE_SHUFFLE_KERNEL(mt_custom_shuffle_idx, shuffle_idx_lane7)
DEFINE_SHUFFLE_KERNEL(mt_custom_shuffle_up, shuffle_up_lane1)
DEFINE_SHUFFLE_KERNEL(mt_custom_shuffle_down, shuffle_down_lane2)
DEFINE_SHUFFLE_KERNEL(mt_custom_shuffle_bfly, shuffle_bfly_lane8)

DEFINE_VCVT_ROUNDTRIP_KERNEL(mt_custom_vcvt_fp16_roundtrip, vcvt_fp16_fp32, vcvt_fp32_fp16, 0.0625f, 0x12u)
DEFINE_VCVT_ROUNDTRIP_KERNEL(mt_custom_vcvt_bf16_roundtrip, vcvt_bf16_fp32, vcvt_fp32_bf16, 0.0625f, 0x34u)

DEFINE_PACKED_BINARY_KERNEL(mt_custom_vadd_f16x2, vadd_f16x2, sample_pack_f16x2_a, sample_pack_f16x2_b)
DEFINE_PACKED_BINARY_KERNEL(mt_custom_vmul_f16x2, vmul_f16x2, sample_pack_f16x2_a, sample_pack_f16x2_b)
DEFINE_PACKED_TERNARY_KERNEL(mt_custom_vfma_f16x2, vfma_f16x2, sample_pack_f16x2_a, sample_pack_f16x2_b, sample_pack_f16x2_c)
DEFINE_PACKED_BINARY_KERNEL(mt_custom_vadd_bf16x2, vadd_bf16x2, sample_pack_bf16x2_a, sample_pack_bf16x2_b)
DEFINE_PACKED_BINARY_KERNEL(mt_custom_vmul_bf16x2, vmul_bf16x2, sample_pack_bf16x2_a, sample_pack_bf16x2_b)
DEFINE_PACKED_TERNARY_KERNEL(mt_custom_vfma_bf16x2, vfma_bf16x2, sample_pack_bf16x2_a, sample_pack_bf16x2_b,
                             sample_pack_bf16x2_c)

DEFINE_SFU_FP32_KERNEL(mt_custom_vex2_f32, vex2_approx_f32, signed_sample(gid, 0x01u, 0.0625f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vlg2_f32, vlg2_approx_f32, positive_sample(gid, 0x02u, 0.03125f, 0.5f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vrcp_f32, vrcp_approx_f32, positive_sample(gid, 0x03u, 0.03125f, 0.5f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vsqrt_f32, vsqrt_approx_f32, positive_sample(gid, 0x04u, 0.03125f, 0.25f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vrsqrt_f32, vrsqrt_approx_f32, positive_sample(gid, 0x05u, 0.03125f, 0.25f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vsin_f32, vsin_approx_f32, signed_sample(gid, 0x06u, 0.0625f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vcos_f32, vcos_approx_f32, signed_sample(gid, 0x07u, 0.0625f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vtanh_f32, vtanh_approx_f32, signed_sample(gid, 0x08u, 0.0625f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vgelu_f32, vgelu_approx_f32, signed_sample(gid, 0x09u, 0.0625f))
DEFINE_SFU_FP32_KERNEL(mt_custom_vsilu_f32, vsilu_approx_f32, signed_sample(gid, 0x0au, 0.0625f))

DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vex2_f16x2, vex2_approx_f16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vrcp_f16x2, vrcp_approx_f16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vsqrt_f16x2, vsqrt_approx_f16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vrsqrt_f16x2, vrsqrt_approx_f16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vtanh_f16x2, vtanh_approx_f16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vgelu_f16x2, vgelu_approx_f16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vsilu_f16x2, vsilu_approx_f16x2)

DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vex2_bf16x2, vex2_approx_bf16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vrcp_bf16x2, vrcp_approx_bf16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vsqrt_bf16x2, vsqrt_approx_bf16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vrsqrt_bf16x2, vrsqrt_approx_bf16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vtanh_bf16x2, vtanh_approx_bf16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vgelu_bf16x2, vgelu_approx_bf16x2)
DEFINE_SFU_PACKED_INPUT_KERNEL(mt_custom_vsilu_bf16x2, vsilu_approx_bf16x2)
