#pragma once

#include "sbt/riscv_decode.hpp"

#include <cstdint>

namespace sbt::ptx::mma {

enum class AbiKey : uint8_t {
  Invalid = 0,
  M16N8K16_F16_F16,
  M16N8K16_F16_F32,
  M16N8K16_BF16_F32,
  M16N8K8_TF32_F32,
};

enum class PackMode : uint8_t {
  Packed16x2 = 0,
  Wide32 = 1,
};

enum class OperandRole : uint8_t {
  A = 0,
  B = 1,
  C = 2,
  D = 3,
};

struct AbiDesc final {
  AbiKey key = AbiKey::Invalid;
  const char *ptx_opcode = nullptr;
  uint8_t m = 0;
  uint8_t n = 0;
  uint8_t k = 0;
  uint8_t a_tuple_regs = 0;
  uint8_t b_tuple_regs = 0;
  uint8_t c_tuple_regs = 0;
  uint8_t d_tuple_regs = 0;
  PackMode a_pack = PackMode::Wide32;
  PackMode b_pack = PackMode::Wide32;
  PackMode c_pack = PackMode::Wide32;
  PackMode d_pack = PackMode::Wide32;
};

struct ScalarTupleValue final {
  uint8_t tuple_reg = 0;
  uint8_t tuple_elem = 0;
  uint8_t tile_row_base = 0;
  uint8_t tile_col_base = 0;
  uint8_t lane_xor_mask = 0;
  uint8_t lane_row_shift = 2;
  uint8_t lane_col_mask = 3;
  uint8_t lane_col_shift = 1;
  uint8_t lane_col_bias = 0;
};

struct BSourceWindowPlan final {
  uint8_t reg_offset = 0;
  uint8_t reg_count = 0;
  uint8_t logical_n_offset = 0;
  uint8_t source_window_n = 0;
  uint8_t native_window_n = 0;
  uint8_t source_window_k = 0;
  uint8_t reg_shift = 0;
  uint8_t lane_shift = 0;
  uint8_t lane_mask = 0;
  uint8_t half_xor = 0;
  bool row_layout = false;
  PackMode source_pack = PackMode::Wide32;
};

const AbiDesc *find_abi_desc(const sbt::MmaInstInfo &mma);
uint8_t shape_m(const sbt::MmaInstInfo &mma);
uint8_t shape_n(const sbt::MmaInstInfo &mma);
uint8_t shape_k(const sbt::MmaInstInfo &mma);
uint8_t packed_values_per_tuple_reg(PackMode pack);
uint8_t tuple_reg_count(const AbiDesc &abi, OperandRole role);
PackMode tuple_pack(const AbiDesc &abi, OperandRole role);
ScalarTupleValue scalar_value_plan(const AbiDesc &abi, OperandRole role, uint8_t value_id);
ScalarTupleValue scalar_tuple_plan(const AbiDesc &abi, OperandRole role, uint8_t tuple_reg, uint8_t tuple_elem);
BSourceWindowPlan b_source_window_plan(const sbt::MmaInstInfo &mma, const AbiDesc &abi, uint8_t slice_col_offset);

} // namespace sbt::ptx::mma
