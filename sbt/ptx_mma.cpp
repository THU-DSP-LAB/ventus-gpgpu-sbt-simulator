#include "sbt/ptx_mma.hpp"

#include <iterator>

namespace sbt::ptx::mma {
namespace {

constexpr AbiDesc kAbiM16N8K16F16F16{
    AbiKey::M16N8K16_F16_F16,
    "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16",
    16,
    8,
    16,
    4,
    2,
    2,
    2,
    PackMode::Packed16x2,
    PackMode::Packed16x2,
    PackMode::Packed16x2,
    PackMode::Packed16x2,
};

constexpr AbiDesc kAbiM16N8K16F16F32{
    AbiKey::M16N8K16_F16_F32,
    "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32",
    16,
    8,
    16,
    4,
    2,
    4,
    4,
    PackMode::Packed16x2,
    PackMode::Packed16x2,
    PackMode::Wide32,
    PackMode::Wide32,
};

constexpr AbiDesc kAbiM16N8K16Bf16F32{
    AbiKey::M16N8K16_BF16_F32,
    "mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32",
    16,
    8,
    16,
    4,
    2,
    4,
    4,
    PackMode::Packed16x2,
    PackMode::Packed16x2,
    PackMode::Wide32,
    PackMode::Wide32,
};

constexpr AbiDesc kAbiM16N8K8Tf32F32{
    AbiKey::M16N8K8_TF32_F32,
    "mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32",
    16,
    8,
    8,
    4,
    2,
    4,
    4,
    PackMode::Wide32,
    PackMode::Wide32,
    PackMode::Wide32,
    PackMode::Wide32,
};

constexpr ScalarTupleValue tuple_value(uint8_t tuple_reg, uint8_t tuple_elem, uint8_t tile_row_base, uint8_t tile_col_base,
                                       uint8_t lane_xor_mask, uint8_t lane_row_shift, uint8_t lane_col_mask,
                                       uint8_t lane_col_shift, uint8_t lane_col_bias) {
  return ScalarTupleValue{tuple_reg, tuple_elem, tile_row_base, tile_col_base, lane_xor_mask, lane_row_shift, lane_col_mask, lane_col_shift,
                          lane_col_bias};
}

constexpr ScalarTupleValue quad_pair_value(uint8_t tuple_reg, uint8_t tuple_elem, uint8_t tile_row_base, uint8_t tile_col_base,
                                           uint8_t lane_col_bias) {
  return tuple_value(tuple_reg, tuple_elem, tile_row_base, tile_col_base, 0u, 2u, 3u, 1u, lane_col_bias);
}

constexpr ScalarTupleValue quad_scalar_value(uint8_t tuple_reg, uint8_t tile_row_base, uint8_t tile_col_base, uint8_t lane_col_bias,
                                             uint8_t lane_xor_mask = 0u) {
  return tuple_value(tuple_reg, 0u, tile_row_base, tile_col_base, lane_xor_mask, 2u, 3u, 1u, lane_col_bias);
}

constexpr ScalarTupleValue row_block_value(uint8_t tuple_reg, uint8_t tile_row_base) {
  return tuple_value(tuple_reg, 0u, tile_row_base, 0u, 0u, 3u, 7u, 0u, 0u);
}

constexpr ScalarTupleValue kF16APlans[] = {
    quad_pair_value(0u, 0u, 0u, 0u, 0u),
    quad_pair_value(0u, 1u, 0u, 0u, 1u),
    quad_pair_value(1u, 0u, 8u, 0u, 0u),
    quad_pair_value(1u, 1u, 8u, 0u, 1u),
    quad_pair_value(2u, 0u, 0u, 8u, 0u),
    quad_pair_value(2u, 1u, 0u, 8u, 1u),
    quad_pair_value(3u, 0u, 8u, 8u, 0u),
    quad_pair_value(3u, 1u, 8u, 8u, 1u),
};

constexpr ScalarTupleValue kF16BPlans[] = {
    quad_pair_value(0u, 0u, 0u, 0u, 0u),
    quad_pair_value(0u, 1u, 0u, 0u, 1u),
    quad_pair_value(1u, 0u, 0u, 8u, 0u),
    quad_pair_value(1u, 1u, 0u, 8u, 1u),
};

constexpr ScalarTupleValue kF16F16CDPlans[] = {
    quad_pair_value(0u, 0u, 0u, 0u, 0u),
    quad_pair_value(0u, 1u, 0u, 0u, 1u),
    quad_pair_value(1u, 0u, 8u, 0u, 0u),
    quad_pair_value(1u, 1u, 8u, 0u, 1u),
};

constexpr ScalarTupleValue kF16F32CDPlans[] = {
    quad_scalar_value(0u, 0u, 0u, 0u),
    quad_scalar_value(1u, 0u, 0u, 1u),
    quad_scalar_value(2u, 8u, 0u, 0u),
    quad_scalar_value(3u, 8u, 0u, 1u),
};

constexpr ScalarTupleValue kBf16APlans[] = {
    quad_pair_value(0u, 0u, 0u, 0u, 0u),
    quad_pair_value(0u, 1u, 0u, 0u, 1u),
    quad_pair_value(1u, 0u, 8u, 0u, 0u),
    quad_pair_value(1u, 1u, 8u, 0u, 1u),
    quad_pair_value(2u, 0u, 0u, 8u, 0u),
    quad_pair_value(2u, 1u, 0u, 8u, 1u),
    quad_pair_value(3u, 0u, 8u, 8u, 0u),
    quad_pair_value(3u, 1u, 8u, 8u, 1u),
};

constexpr ScalarTupleValue kBf16BPlans[] = {
    quad_pair_value(0u, 0u, 0u, 0u, 0u),
    quad_pair_value(0u, 1u, 0u, 0u, 1u),
    quad_pair_value(1u, 0u, 0u, 8u, 0u),
    quad_pair_value(1u, 1u, 0u, 8u, 1u),
};

constexpr ScalarTupleValue kBf16CPlans[] = {
    quad_scalar_value(0u, 0u, 0u, 0u),
    quad_scalar_value(1u, 0u, 0u, 1u),
    quad_scalar_value(2u, 8u, 0u, 0u),
    quad_scalar_value(3u, 8u, 0u, 1u),
};

constexpr ScalarTupleValue kBf16DPlans[] = {
    quad_scalar_value(0u, 0u, 0u, 0u),
    quad_scalar_value(1u, 0u, 0u, 1u),
    quad_scalar_value(2u, 8u, 0u, 0u),
    quad_scalar_value(3u, 8u, 0u, 1u),
};

constexpr ScalarTupleValue kTf32APlans[] = {
    tuple_value(0u, 0u, 0u, 0u, 0u, 2u, 3u, 0u, 0u),
    tuple_value(1u, 0u, 8u, 0u, 0u, 2u, 3u, 0u, 0u),
    tuple_value(2u, 0u, 0u, 4u, 0u, 2u, 3u, 0u, 0u),
    tuple_value(3u, 0u, 8u, 4u, 0u, 2u, 3u, 0u, 0u),
};

constexpr ScalarTupleValue kTf32BPlans[] = {
    tuple_value(0u, 0u, 0u, 0u, 0u, 2u, 3u, 0u, 0u),
    tuple_value(1u, 0u, 0u, 4u, 0u, 2u, 3u, 0u, 0u),
};

constexpr ScalarTupleValue kTf32CDPlans[] = {
    quad_scalar_value(0u, 0u, 0u, 0u),
    quad_scalar_value(1u, 0u, 0u, 1u),
    quad_scalar_value(2u, 8u, 0u, 0u),
    quad_scalar_value(3u, 8u, 0u, 1u),
};

const ScalarTupleValue *plans_for(const AbiDesc &abi, OperandRole role, uint8_t &count) {
  switch (abi.key) {
  case AbiKey::M16N8K16_F16_F16:
    switch (role) {
    case OperandRole::A: count = static_cast<uint8_t>(std::size(kF16APlans)); return kF16APlans;
    case OperandRole::B: count = static_cast<uint8_t>(std::size(kF16BPlans)); return kF16BPlans;
    case OperandRole::C:
    case OperandRole::D: count = static_cast<uint8_t>(std::size(kF16F16CDPlans)); return kF16F16CDPlans;
    }
    break;
  case AbiKey::M16N8K16_F16_F32:
    switch (role) {
    case OperandRole::A: count = static_cast<uint8_t>(std::size(kF16APlans)); return kF16APlans;
    case OperandRole::B: count = static_cast<uint8_t>(std::size(kF16BPlans)); return kF16BPlans;
    case OperandRole::C:
    case OperandRole::D: count = static_cast<uint8_t>(std::size(kF16F32CDPlans)); return kF16F32CDPlans;
    }
    break;
  case AbiKey::M16N8K16_BF16_F32:
    switch (role) {
    case OperandRole::A: count = static_cast<uint8_t>(std::size(kBf16APlans)); return kBf16APlans;
    case OperandRole::B: count = static_cast<uint8_t>(std::size(kBf16BPlans)); return kBf16BPlans;
    case OperandRole::C: count = static_cast<uint8_t>(std::size(kBf16CPlans)); return kBf16CPlans;
    case OperandRole::D: count = static_cast<uint8_t>(std::size(kBf16DPlans)); return kBf16DPlans;
    }
    break;
  case AbiKey::M16N8K8_TF32_F32:
    switch (role) {
    case OperandRole::A: count = static_cast<uint8_t>(std::size(kTf32APlans)); return kTf32APlans;
    case OperandRole::B: count = static_cast<uint8_t>(std::size(kTf32BPlans)); return kTf32BPlans;
    case OperandRole::C:
    case OperandRole::D: count = static_cast<uint8_t>(std::size(kTf32CDPlans)); return kTf32CDPlans;
    }
    break;
  case AbiKey::Invalid: break;
  }
  count = 0u;
  return nullptr;
}

} // namespace

const AbiDesc *find_abi_desc(const sbt::MmaInstInfo &mma) {
  switch (mma.ab_type) {
  case sbt::MmaAbType::Fp16:
    if (shape_k(mma) != 16u) return nullptr;
    if (mma.cd_type == sbt::MmaCdType::Fp16) return &kAbiM16N8K16F16F16;
    if (mma.cd_type == sbt::MmaCdType::Fp32) return &kAbiM16N8K16F16F32;
    return nullptr;
  case sbt::MmaAbType::Bf16:
    if (shape_k(mma) != 16u || mma.cd_type != sbt::MmaCdType::Fp32) return nullptr;
    return &kAbiM16N8K16Bf16F32;
  case sbt::MmaAbType::Tf32:
    if (shape_k(mma) != 8u || mma.cd_type != sbt::MmaCdType::Fp32) return nullptr;
    return &kAbiM16N8K8Tf32F32;
  case sbt::MmaAbType::None: return nullptr;
  }
  return nullptr;
}

uint8_t shape_m(const sbt::MmaInstInfo &mma) {
  switch (mma.shape) {
  case sbt::MmaShape::M8N8K16:
  case sbt::MmaShape::M8N16K16:
  case sbt::MmaShape::M8N8K8:
  case sbt::MmaShape::M8N16K8: return 8u;
  case sbt::MmaShape::M16N8K16:
  case sbt::MmaShape::M16N16K16:
  case sbt::MmaShape::M16N8K8:
  case sbt::MmaShape::M16N16K8: return 16u;
  default: return 0u;
  }
}

uint8_t shape_n(const sbt::MmaInstInfo &mma) {
  switch (mma.shape) {
  case sbt::MmaShape::M16N8K16:
  case sbt::MmaShape::M16N8K8: return 8u;
  case sbt::MmaShape::M16N16K16:
  case sbt::MmaShape::M16N16K8: return 16u;
  default: return 0u;
  }
}

uint8_t shape_k(const sbt::MmaInstInfo &mma) {
  switch (mma.shape) {
  case sbt::MmaShape::M16N8K16:
  case sbt::MmaShape::M16N16K16: return 16u;
  case sbt::MmaShape::M16N8K8:
  case sbt::MmaShape::M16N16K8: return 8u;
  default: return 0u;
  }
}

uint8_t packed_values_per_tuple_reg(PackMode pack) {
  return pack == PackMode::Packed16x2 ? 2u : 1u;
}

uint8_t tuple_reg_count(const AbiDesc &abi, OperandRole role) {
  switch (role) {
  case OperandRole::A: return abi.a_tuple_regs;
  case OperandRole::B: return abi.b_tuple_regs;
  case OperandRole::C: return abi.c_tuple_regs;
  case OperandRole::D: return abi.d_tuple_regs;
  }
  return 0u;
}

PackMode tuple_pack(const AbiDesc &abi, OperandRole role) {
  switch (role) {
  case OperandRole::A: return abi.a_pack;
  case OperandRole::B: return abi.b_pack;
  case OperandRole::C: return abi.c_pack;
  case OperandRole::D: return abi.d_pack;
  }
  return PackMode::Wide32;
}

ScalarTupleValue scalar_value_plan(const AbiDesc &abi, OperandRole role, uint8_t value_id) {
  uint8_t count = 0u;
  const auto *plans = plans_for(abi, role, count);
  if (plans == nullptr || value_id >= count) return {};
  return plans[value_id];
}

ScalarTupleValue scalar_tuple_plan(const AbiDesc &abi, OperandRole role, uint8_t tuple_reg, uint8_t tuple_elem) {
  uint8_t count = 0u;
  const auto *plans = plans_for(abi, role, count);
  if (plans == nullptr) return {};
  for (uint8_t i = 0; i < count; ++i) {
    if (plans[i].tuple_reg == tuple_reg && plans[i].tuple_elem == tuple_elem) return plans[i];
  }
  return {};
}

BSourceWindowPlan b_source_window_plan(const sbt::MmaInstInfo &mma, const AbiDesc &abi, uint8_t slice_col_offset) {
  BSourceWindowPlan out;
  out.reg_count = mma.b_regs_per_thread;
  out.logical_n_offset = slice_col_offset;
  out.source_window_n = shape_n(mma);
  out.source_window_k = shape_k(mma);
  out.native_window_n = abi.n;
  out.row_layout = mma.spike_b_row_layout;
  out.source_pack = abi.b_pack;
  out.reg_shift = (abi.b_pack == PackMode::Packed16x2) ? 6u : 5u;
  out.lane_shift = (abi.b_pack == PackMode::Packed16x2) ? 1u : 0u;
  out.lane_mask = 31u;
  out.half_xor = 0u;
  return out;
}

} // namespace sbt::ptx::mma
