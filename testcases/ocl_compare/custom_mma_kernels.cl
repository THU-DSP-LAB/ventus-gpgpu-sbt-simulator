// OpenCL micro-tests for MMA custom instructions.
//
// Convention:
//   __kernel void K(__global const uint *A, __global uint *B)
// with A and B each containing N uint32 elements.
//
// This file is intentionally small and serves as the MMA microtest family seed.
// It currently covers committed non-fp16->fp16 first-batch families plus one
// fp16->fp16 blocked probe kernel.
//
// Optional feature macros (enabled by tools/custom_mma_oracle.py per-kernel):
// - SBT_MMA_ENABLE_F16_M16N8K16
// - SBT_MMA_ENABLE_BF16_M16N8K16
// - SBT_MMA_ENABLE_TF32_M16N8K8
// - SBT_MMA_ENABLE_F16_M16N16K16
// - SBT_MMA_ENABLE_BF16_M16N16K16
// - SBT_MMA_ENABLE_TF32_M16N16K8
// - SBT_MMA_ENABLE_FP16_FP16_BLOCKED

typedef unsigned int uint;
typedef uint uint2 __attribute__((ext_vector_type(2)));
typedef uint uint4 __attribute__((ext_vector_type(4)));
typedef uint uint8 __attribute__((ext_vector_type(8)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef float float8 __attribute__((ext_vector_type(8)));

// Keep the MMA microtest input carriers branch-free so the gate exercises MMA
// lowering rather than unrelated vector branch/join helper lowering.
__constant uint kFiniteF16Bits[8] = {
    0x3c00u,
    0xbc00u,
    0x3800u,
    0x4000u,
    0x3400u,
    0xc000u,
    0x3e00u,
    0xb800u,
};

__constant uint kFiniteBf16Bits[8] = {
    0x3f80u,
    0xbf80u,
    0x3f00u,
    0x4000u,
    0x3e80u,
    0xc000u,
    0x3fc0u,
    0xbf00u,
};

__constant uint kFiniteTf32Words[8] = {
    0x3f800000u,
    0xbf800000u,
    0x3f000000u,
    0x40000000u,
    0x3e800000u,
    0xc0000000u,
    0x3fc00000u,
    0xbf000000u,
};

static inline uint pack_u16x2(uint lo, uint hi) {
  return (lo & 0xffffu) | ((hi & 0xffffu) << 16);
}

static inline uint sample_lane_index(uint seed, uint salt) {
  return (((seed >> ((salt & 3u) * 5u)) ^ (salt * 13u)) & 7u);
}

static inline uint finite_f16_bits(uint idx) {
  return kFiniteF16Bits[idx & 7u];
}

static inline uint finite_bf16_bits(uint idx) {
  return kFiniteBf16Bits[idx & 7u];
}

static inline uint finite_tf32_word(uint idx) {
  return kFiniteTf32Words[idx & 7u];
}

static inline uint sample_packed_f16(uint seed, uint salt_lo, uint salt_hi) {
  return pack_u16x2(finite_f16_bits(sample_lane_index(seed, salt_lo)), finite_f16_bits(sample_lane_index(seed, salt_hi)));
}

static inline uint sample_packed_bf16(uint seed, uint salt_lo, uint salt_hi) {
  return pack_u16x2(finite_bf16_bits(sample_lane_index(seed, salt_lo)), finite_bf16_bits(sample_lane_index(seed, salt_hi)));
}

static inline uint sample_tf32(uint seed, uint salt) {
  return finite_tf32_word(sample_lane_index(seed, salt));
}

static inline float signed_sample(uint gid, uint salt, float scale) {
  const int s = (int)(((gid * 37u) ^ salt) & 0x7fu) - 64;
  return ((float)s) * scale;
}

#if defined(SBT_MMA_ENABLE_F16_M16N8K16)
static inline float4 mma_m16n8k16_row_col_f32_f16_f16_f32(uint4 a, uint2 b, float4 c) {
  return __builtin_riscv_ventus_mma_m16n8k16_row_col_f32_f16_f16_f32(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_BF16_M16N8K16)
static inline float4 mma_m16n8k16_row_col_f32_bf16_bf16_f32(uint4 a, uint2 b, float4 c) {
  return __builtin_riscv_ventus_mma_m16n8k16_row_col_f32_bf16_bf16_f32(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_TF32_M16N8K8)
static inline float4 mma_m16n8k8_row_col_f32_tf32_tf32_f32(uint4 a, uint2 b, float4 c) {
  return __builtin_riscv_ventus_mma_m16n8k8_row_col_f32_tf32_tf32_f32(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_F16_M16N16K16)
static inline float8 mma_m16n16k16_row_col_f32_f16_f16_f32(uint4 a, uint4 b, float8 c) {
  return __builtin_riscv_ventus_mma_m16n16k16_row_col_f32_f16_f16_f32(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_BF16_M16N16K16)
static inline float8 mma_m16n16k16_row_col_f32_bf16_bf16_f32(uint4 a, uint4 b, float8 c) {
  return __builtin_riscv_ventus_mma_m16n16k16_row_col_f32_bf16_bf16_f32(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_TF32_M16N16K8)
static inline float8 mma_m16n16k8_row_col_f32_tf32_tf32_f32(uint4 a, uint4 b, float8 c) {
  return __builtin_riscv_ventus_mma_m16n16k8_row_col_f32_tf32_tf32_f32(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_FP16_FP16_BLOCKED)
static inline uint2 mma_m16n8k16_row_col_f16_f16_f16_f16(uint4 a, uint2 b, uint2 c) {
  return __builtin_riscv_ventus_mma_m16n8k16_row_col_f16_f16_f16_f16(a, b, c);
}
#endif

#if defined(SBT_MMA_ENABLE_F16_M16N8K16)
__kernel void mt_custom_mma_m16n8k16_row_col_f32_f16_f16_f32(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = A[gid] ^ (gid * 0x9e3779b9u);

  const uint4 a = (uint4)(sample_packed_f16(seed, 0u, 1u), sample_packed_f16(seed, 2u, 3u), sample_packed_f16(seed, 4u, 5u),
                          sample_packed_f16(seed, 6u, 7u));
  const uint2 b = (uint2)(sample_packed_f16(seed ^ 0x13579bdfu, 1u, 3u), sample_packed_f16(seed ^ 0x2468ace0u, 5u, 7u));
  const float4 c = (float4)(signed_sample(gid, 0x11u, 0.0625f), signed_sample(gid, 0x22u, 0.03125f),
                            signed_sample(gid, 0x33u, 0.015625f), signed_sample(gid, 0x44u, 0.125f));

  const float4 d = mma_m16n8k16_row_col_f32_f16_f16_f32(a, b, c);

  // Expose all output fragment components over different lanes.
  float out = d.x;
  if ((gid & 3u) == 1u) out = d.y;
  else if ((gid & 3u) == 2u) out = d.z;
  else if ((gid & 3u) == 3u) out = d.w;
  B[gid] = as_uint(out);
}
#endif

#if defined(SBT_MMA_ENABLE_BF16_M16N8K16)
__kernel void mt_custom_mma_m16n8k16_row_col_f32_bf16_bf16_f32(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = (A[gid] << 1) ^ (gid * 0x85ebca6bu);

  const uint4 a = (uint4)(sample_packed_bf16(seed, 0u, 1u), sample_packed_bf16(seed, 2u, 3u), sample_packed_bf16(seed, 4u, 5u),
                          sample_packed_bf16(seed, 6u, 7u));
  const uint2 b = (uint2)(sample_packed_bf16(seed ^ 0x55aa55aau, 1u, 3u), sample_packed_bf16(seed ^ 0xaa55aa55u, 5u, 7u));
  const float4 c = (float4)(signed_sample(gid, 0x51u, 0.0625f), signed_sample(gid, 0x62u, 0.03125f),
                            signed_sample(gid, 0x73u, 0.015625f), signed_sample(gid, 0x84u, 0.125f));

  const float4 d = mma_m16n8k16_row_col_f32_bf16_bf16_f32(a, b, c);
  float out = d.x;
  if ((gid & 3u) == 1u) out = d.y;
  else if ((gid & 3u) == 2u) out = d.z;
  else if ((gid & 3u) == 3u) out = d.w;
  B[gid] = as_uint(out);
}
#endif

#if defined(SBT_MMA_ENABLE_TF32_M16N8K8)
__kernel void mt_custom_mma_m16n8k8_row_col_f32_tf32_tf32_f32(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = (A[gid] * 17u) ^ (gid * 0xc2b2ae35u);

  const uint4 a = (uint4)(sample_tf32(seed, 0u), sample_tf32(seed, 1u), sample_tf32(seed, 2u), sample_tf32(seed, 3u));
  const uint2 b = (uint2)(sample_tf32(seed ^ 0x31415926u, 4u), sample_tf32(seed ^ 0x27182818u, 5u));
  const float4 c = (float4)(signed_sample(gid, 0x91u, 0.0625f), signed_sample(gid, 0xa2u, 0.03125f),
                            signed_sample(gid, 0xb3u, 0.015625f), signed_sample(gid, 0xc4u, 0.125f));

  const float4 d = mma_m16n8k8_row_col_f32_tf32_tf32_f32(a, b, c);
  float out = d.x;
  if ((gid & 3u) == 1u) out = d.y;
  else if ((gid & 3u) == 2u) out = d.z;
  else if ((gid & 3u) == 3u) out = d.w;
  B[gid] = as_uint(out);
}
#endif

#if defined(SBT_MMA_ENABLE_F16_M16N16K16) || defined(SBT_MMA_ENABLE_BF16_M16N16K16) || defined(SBT_MMA_ENABLE_TF32_M16N16K8)
static inline float pick_float8_lane(float8 v, uint gid) {
  const uint lane = gid & 7u;
  if (lane == 0u) return v.s0;
  if (lane == 1u) return v.s1;
  if (lane == 2u) return v.s2;
  if (lane == 3u) return v.s3;
  if (lane == 4u) return v.s4;
  if (lane == 5u) return v.s5;
  if (lane == 6u) return v.s6;
  return v.s7;
}
#endif

#if defined(SBT_MMA_ENABLE_F16_M16N16K16)
__kernel void mt_custom_mma_m16n16k16_row_col_f32_f16_f16_f32(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = (A[gid] * 31u) ^ (gid * 0x9e3779b9u);

  const uint4 a = (uint4)(sample_packed_f16(seed, 0u, 1u), sample_packed_f16(seed, 2u, 3u), sample_packed_f16(seed, 4u, 5u),
                          sample_packed_f16(seed, 6u, 7u));
  const uint4 b = (uint4)(sample_packed_f16(seed ^ 0x11111111u, 1u, 2u), sample_packed_f16(seed ^ 0x22222222u, 3u, 4u),
                          sample_packed_f16(seed ^ 0x33333333u, 5u, 6u), sample_packed_f16(seed ^ 0x44444444u, 7u, 0u));
  const float8 c = (float8)(signed_sample(gid, 0x12u, 0.0625f), signed_sample(gid, 0x23u, 0.03125f),
                            signed_sample(gid, 0x34u, 0.015625f), signed_sample(gid, 0x45u, 0.125f),
                            signed_sample(gid, 0x56u, 0.0625f), signed_sample(gid, 0x67u, 0.03125f),
                            signed_sample(gid, 0x78u, 0.015625f), signed_sample(gid, 0x89u, 0.125f));

  const float8 d = mma_m16n16k16_row_col_f32_f16_f16_f32(a, b, c);
  B[gid] = as_uint(pick_float8_lane(d, gid));
}
#endif

#if defined(SBT_MMA_ENABLE_BF16_M16N16K16)
__kernel void mt_custom_mma_m16n16k16_row_col_f32_bf16_bf16_f32(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = (A[gid] * 13u) ^ (gid * 0x165667b1u);

  const uint4 a = (uint4)(sample_packed_bf16(seed, 0u, 1u), sample_packed_bf16(seed, 2u, 3u), sample_packed_bf16(seed, 4u, 5u),
                          sample_packed_bf16(seed, 6u, 7u));
  const uint4 b = (uint4)(sample_packed_bf16(seed ^ 0x0f0f0f0fu, 1u, 2u), sample_packed_bf16(seed ^ 0xf0f0f0f0u, 3u, 4u),
                          sample_packed_bf16(seed ^ 0x55aa55aau, 5u, 6u), sample_packed_bf16(seed ^ 0xaa55aa55u, 7u, 0u));
  const float8 c = (float8)(signed_sample(gid, 0x1fu, 0.0625f), signed_sample(gid, 0x2eu, 0.03125f),
                            signed_sample(gid, 0x3du, 0.015625f), signed_sample(gid, 0x4cu, 0.125f),
                            signed_sample(gid, 0x5bu, 0.0625f), signed_sample(gid, 0x6au, 0.03125f),
                            signed_sample(gid, 0x79u, 0.015625f), signed_sample(gid, 0x88u, 0.125f));

  const float8 d = mma_m16n16k16_row_col_f32_bf16_bf16_f32(a, b, c);
  B[gid] = as_uint(pick_float8_lane(d, gid));
}
#endif

#if defined(SBT_MMA_ENABLE_TF32_M16N16K8)
__kernel void mt_custom_mma_m16n16k8_row_col_f32_tf32_tf32_f32(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = (A[gid] * 7u) ^ (gid * 0x27d4eb2fu);

  const uint4 a = (uint4)(sample_tf32(seed, 0u), sample_tf32(seed, 1u), sample_tf32(seed, 2u), sample_tf32(seed, 3u));
  const uint4 b = (uint4)(sample_tf32(seed ^ 0x01020304u, 4u), sample_tf32(seed ^ 0x11121314u, 5u), sample_tf32(seed ^ 0x21222324u, 6u),
                          sample_tf32(seed ^ 0x31323334u, 7u));
  const float8 c = (float8)(signed_sample(gid, 0x16u, 0.0625f), signed_sample(gid, 0x27u, 0.03125f),
                            signed_sample(gid, 0x38u, 0.015625f), signed_sample(gid, 0x49u, 0.125f),
                            signed_sample(gid, 0x5au, 0.0625f), signed_sample(gid, 0x6bu, 0.03125f),
                            signed_sample(gid, 0x7cu, 0.015625f), signed_sample(gid, 0x8du, 0.125f));

  const float8 d = mma_m16n16k8_row_col_f32_tf32_tf32_f32(a, b, c);
  B[gid] = as_uint(pick_float8_lane(d, gid));
}
#endif

// This kernel is intentionally kept as a blocked probe. sbtsim currently keeps
// fp16->fp16 MMA lowering on explicit fail-fast due to toolchain contract mismatch.
#if defined(SBT_MMA_ENABLE_FP16_FP16_BLOCKED)
__kernel void mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16_blocked(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint seed = (A[gid] * 19u) ^ (gid * 0x7f4a7c15u);

  const uint4 a = (uint4)(seed + 0x00110011u, seed + 0x00220022u, seed + 0x00330033u, seed + 0x00440044u);
  const uint2 b = (uint2)(seed + 0x00550055u, seed + 0x00660066u);
  const uint2 c = (uint2)(seed + 0x00770077u, seed + 0x00880088u);
  const uint2 d = mma_m16n8k16_row_col_f16_f16_f16_f16(a, b, c);
  B[gid] = ((gid & 1u) == 0u) ? d.x : d.y;
}
#endif
