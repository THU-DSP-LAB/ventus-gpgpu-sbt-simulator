#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

bool try_emit_mma(EmitCtx &ctx, const sbt::DecodedInst &di) {
  if (di.emit.domain != EmitDomain::Mma) return false;
  ctx.emit_mma_inst(di);
  return true;
}

std::string EmitCtx::mma_detail(const sbt::MmaInstInfo &mma) const {
  return "shape=" + std::string(sbt::to_string(mma.shape)) + " layout=" + std::string(sbt::to_string(mma.a_layout)) + "." +
         std::string(sbt::to_string(mma.b_layout)) + " ab=" + std::string(sbt::to_string(mma.ab_type)) + " cd=" +
         std::string(sbt::to_string(mma.cd_type)) + " support=" + std::string(sbt::to_string(mma.support_class)) + " lowering=" +
         std::string(sbt::to_string(mma.lowering_class));
}

void EmitCtx::emit_require_full_warp_for_mma(uint32_t pc_for_err) {
  const std::string not_full = tmp_pred();
  (void)pc_for_err;
  emit_read_activemask(r(1));
  emit_line("setp.ne.u32 " + not_full + ", " + r(1) + ", " + hex_u32(kFullWarpMask) + ";");
  emit_line("@" + not_full + " trap;");
}

void EmitCtx::emit_shfl_idx_b32(const std::string &dst_r, const std::string &src_r, const std::string &lane_r) {
  emit_line("shfl.sync.idx.b32 " + dst_r + ", " + src_r + ", " + lane_r + ", 0x1f, " + r(1) + ";");
}

void EmitCtx::emit_select_u32_by_index(const std::string &dst_r, const std::vector<std::string> &candidates, const std::string &idx_r) {
  require(!candidates.empty(), EmitError("invalid.mma.shuffle_select", func_name, 0, "empty candidate list"));
  const std::string out_of_range = tmp_pred();
  emit_line("setp.ge.u32 " + out_of_range + ", " + idx_r + ", " + std::to_string(candidates.size()) + ";");
  emit_line("@" + out_of_range + " trap;");
  emit_line("mov.u32 " + dst_r + ", " + candidates[0] + ";");
  for (size_t i = 1; i < candidates.size(); ++i) {
    const std::string take = tmp_pred();
    emit_line("setp.eq.u32 " + take + ", " + idx_r + ", " + std::to_string(i) + ";");
    emit_line("@" + take + " mov.u32 " + dst_r + ", " + candidates[i] + ";");
  }
}

void EmitCtx::emit_gather_v_window_word_by_shuffle(int base_reg, uint8_t reg_count, const std::string &source_reg_r,
                                          const std::string &source_lane_r, const std::string &dst_r) {
  std::vector<std::string> candidates;
  candidates.reserve(reg_count);
  for (uint8_t reg = 0; reg < reg_count; ++reg) {
    const std::string candidate = tmp_b32();
    emit_shfl_idx_b32(candidate, v(base_reg + static_cast<int>(reg)), source_lane_r);
    candidates.push_back(candidate);
  }
  emit_select_u32_by_index(dst_r, candidates, source_reg_r);
}

void EmitCtx::emit_compute_tuple_logical_coord(const sbt::ptx::mma::ScalarTupleValue &value, const std::string &row_r, const std::string &col_r) {
  const std::string lane_r = (value.lane_xor_mask == 0u) ? r(0) : col_r;
  if (value.lane_xor_mask != 0u) emit_line("xor.b32 " + lane_r + ", " + r(0) + ", " + std::to_string(value.lane_xor_mask) + ";");
  emit_line("shr.u32 " + row_r + ", " + lane_r + ", " + std::to_string(value.lane_row_shift) + ";");
  if (value.tile_row_base != 0u) emit_line("add.u32 " + row_r + ", " + row_r + ", " + std::to_string(value.tile_row_base) + ";");
  emit_line("and.b32 " + col_r + ", " + lane_r + ", " + std::to_string(value.lane_col_mask) + ";");
  if (value.lane_col_shift != 0u) emit_line("shl.b32 " + col_r + ", " + col_r + ", " + std::to_string(value.lane_col_shift) + ";");
  const uint32_t col_bias = static_cast<uint32_t>(value.tile_col_base) + static_cast<uint32_t>(value.lane_col_bias);
  if (col_bias != 0u) emit_line("add.u32 " + col_r + ", " + col_r + ", " + std::to_string(col_bias) + ";");
}

void EmitCtx::emit_compute_window_index_from_logical_coord(const sbt::DecodedInst &di, sbt::ptx::mma::OperandRole role, uint8_t slice_col_offset,
                                                  const std::string &logical_row_r, const std::string &logical_col_r,
                                                  const std::string &idx_r, const std::string &tmp_r) {
  switch (role) {
  case sbt::ptx::mma::OperandRole::A:
    if (di.mma.spike_a_column_layout) {
      emit_line("mul.lo.u32 " + idx_r + ", " + logical_col_r + ", " + std::to_string(sbt::ptx::mma::shape_m(di.mma)) + ";");
      emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + logical_row_r + ";");
    } else {
      emit_line("mul.lo.u32 " + idx_r + ", " + logical_row_r + ", " + std::to_string(sbt::ptx::mma::shape_k(di.mma)) + ";");
      emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + logical_col_r + ";");
    }
    return;
  case sbt::ptx::mma::OperandRole::C:
  case sbt::ptx::mma::OperandRole::D:
    emit_line("mov.u32 " + tmp_r + ", " + logical_col_r + ";");
    if (slice_col_offset != 0u) emit_line("add.u32 " + tmp_r + ", " + tmp_r + ", " + std::to_string(slice_col_offset) + ";");
    emit_line("mul.lo.u32 " + idx_r + ", " + logical_row_r + ", " + std::to_string(sbt::ptx::mma::shape_n(di.mma)) + ";");
    emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + tmp_r + ";");
    return;
  case sbt::ptx::mma::OperandRole::B:
    return;
  }
}

void EmitCtx::emit_compute_b_window_index_from_logical_coord(const sbt::DecodedInst &di, const sbt::ptx::mma::BSourceWindowPlan &plan,
                                                    const std::string &logical_n_r, const std::string &logical_k_r, const std::string &idx_r,
                                                    const std::string &tmp_r) {
  (void)di;
  emit_line("mov.u32 " + tmp_r + ", " + logical_n_r + ";");
  if (plan.logical_n_offset != 0u) emit_line("add.u32 " + tmp_r + ", " + tmp_r + ", " + std::to_string(plan.logical_n_offset) + ";");
  if (plan.row_layout) {
    emit_line("mul.lo.u32 " + idx_r + ", " + tmp_r + ", " + std::to_string(plan.source_window_k) + ";");
    emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + logical_k_r + ";");
    return;
  }
  emit_line("mul.lo.u32 " + idx_r + ", " + logical_k_r + ", " + std::to_string(plan.source_window_n) + ";");
  emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + tmp_r + ";");
}

void EmitCtx::emit_materialize_scalar_value_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi,
                                              sbt::ptx::mma::OperandRole role, uint8_t slice_col_offset,
                                              const sbt::ptx::mma::ScalarTupleValue &value, int base_reg, uint8_t window_reg_count,
                                              const std::string &dst_r) {
  const std::string row_r = tmp_b32();
  const std::string col_r = tmp_b32();
  const std::string idx_r = tmp_b32();
  const std::string reg_r = tmp_b32();
  const std::string lane_r = tmp_b32();
  const std::string tmp_r = tmp_b32();
  const std::string tmp2_r = tmp_b32();
  emit_compute_tuple_logical_coord(value, row_r, col_r);
  emit_compute_window_index_from_logical_coord(di, role, slice_col_offset, row_r, col_r, idx_r, tmp_r);

  if ((role == sbt::ptx::mma::OperandRole::C || role == sbt::ptx::mma::OperandRole::D) &&
      sbt::ptx::mma::tuple_pack(abi, role) == sbt::ptx::mma::PackMode::Packed16x2) {
    const std::string take_hi = tmp_pred();
    emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 6;");
    emit_line("shr.u32 " + lane_r + ", " + idx_r + ", 1;");
    emit_line("and.b32 " + lane_r + ", " + lane_r + ", 31;");
    emit_gather_v_window_word_by_shuffle(base_reg, window_reg_count, reg_r, lane_r, dst_r);
    emit_line("and.b32 " + tmp2_r + ", " + idx_r + ", 1;");
    emit_line("setp.ne.u32 " + take_hi + ", " + tmp2_r + ", 0;");
    emit_line("@" + take_hi + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
    emit_line("and.b32 " + dst_r + ", " + dst_r + ", 0xffff;");
    return;
  }

  if (role == sbt::ptx::mma::OperandRole::A || role == sbt::ptx::mma::OperandRole::B) {
    if (di.mma.wide_ab) {
      emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 5;");
      emit_line("and.b32 " + lane_r + ", " + idx_r + ", 31;");
      emit_gather_v_window_word_by_shuffle(base_reg, window_reg_count, reg_r, lane_r, dst_r);
      return;
    }
    emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 6;");
    emit_line("shr.u32 " + lane_r + ", " + idx_r + ", 1;");
    emit_line("and.b32 " + lane_r + ", " + lane_r + ", 31;");
    emit_gather_v_window_word_by_shuffle(base_reg, window_reg_count, reg_r, lane_r, dst_r);
    emit_line("and.b32 " + tmp2_r + ", " + idx_r + ", 1;");
    const std::string take_hi = tmp_pred();
    emit_line("setp.ne.u32 " + take_hi + ", " + tmp2_r + ", 0;");
    emit_line("@" + take_hi + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
    emit_line("and.b32 " + dst_r + ", " + dst_r + ", 0xffff;");
    return;
  }

  emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 5;");
  emit_line("and.b32 " + lane_r + ", " + idx_r + ", 31;");
  emit_gather_v_window_word_by_shuffle(base_reg, window_reg_count, reg_r, lane_r, dst_r);
}

void EmitCtx::emit_materialize_b_scalar_value_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi,
                                                const sbt::ptx::mma::BSourceWindowPlan &plan,
                                                const sbt::ptx::mma::ScalarTupleValue &value, int base_reg,
                                                const std::string &dst_r) {
  const std::string row_r = tmp_b32();
  const std::string col_r = tmp_b32();
  const std::string idx_r = tmp_b32();
  const std::string reg_r = tmp_b32();
  const std::string lane_r = tmp_b32();
  const std::string tmp_r = tmp_b32();
  const std::string tmp2_r = tmp_b32();
  (void)abi;
  emit_compute_tuple_logical_coord(value, row_r, col_r);
  emit_compute_b_window_index_from_logical_coord(di, plan, row_r, col_r, idx_r, tmp_r);

  if (plan.source_pack == sbt::ptx::mma::PackMode::Wide32) {
    emit_line("shr.u32 " + reg_r + ", " + idx_r + ", " + std::to_string(plan.reg_shift) + ";");
    emit_line("and.b32 " + lane_r + ", " + idx_r + ", " + std::to_string(plan.lane_mask) + ";");
    emit_gather_v_window_word_by_shuffle(base_reg, plan.reg_count, reg_r, lane_r, dst_r);
    return;
  }

  emit_line("shr.u32 " + reg_r + ", " + idx_r + ", " + std::to_string(plan.reg_shift) + ";");
  emit_line("shr.u32 " + lane_r + ", " + idx_r + ", " + std::to_string(plan.lane_shift) + ";");
  emit_line("and.b32 " + lane_r + ", " + lane_r + ", " + std::to_string(plan.lane_mask) + ";");
  emit_gather_v_window_word_by_shuffle(base_reg, plan.reg_count, reg_r, lane_r, dst_r);
  emit_line("and.b32 " + tmp2_r + ", " + idx_r + ", 1;");
  if (plan.half_xor != 0u) emit_line("xor.b32 " + tmp2_r + ", " + tmp2_r + ", " + std::to_string(plan.half_xor) + ";");
  const std::string take_hi = tmp_pred();
  emit_line("setp.ne.u32 " + take_hi + ", " + tmp2_r + ", 0;");
  emit_line("@" + take_hi + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
  emit_line("and.b32 " + dst_r + ", " + dst_r + ", 0xffff;");
}

void EmitCtx::emit_materialize_tuple_regs(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, sbt::ptx::mma::OperandRole role,
                                 uint8_t slice_col_offset, int base_reg, uint8_t window_reg_count, const std::vector<std::string> &dst_regs) {
  const auto pack = sbt::ptx::mma::tuple_pack(abi, role);
  const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, role);
  require(dst_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));
  require(role != sbt::ptx::mma::OperandRole::B, EmitError("invalid.mma.plan", func_name, di.pc, mma_detail(di.mma)));

  if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
    for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
      const std::string lo_word = tmp_b32();
      const std::string hi_word = tmp_b32();
      for (uint8_t elem = 0; elem < 2u; ++elem) {
        const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, role, tuple_reg, elem);
        const std::string dst_word = (elem == 0u) ? lo_word : hi_word;
        emit_materialize_scalar_value_by_shuffle(di, abi, role, slice_col_offset, value, base_reg, window_reg_count, dst_word);
        emit_line("and.b32 " + dst_word + ", " + dst_word + ", 0xffff;");
      }
      emit_line("shl.b32 " + hi_word + ", " + hi_word + ", 16;");
      emit_line("or.b32 " + dst_regs[tuple_reg] + ", " + lo_word + ", " + hi_word + ";");
    }
    return;
  }

  for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
    const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, role, tuple_reg, 0u);
    const std::string value_word = tmp_b32();
    emit_materialize_scalar_value_by_shuffle(di, abi, role, slice_col_offset, value, base_reg, window_reg_count, value_word);
    if (role == sbt::ptx::mma::OperandRole::C || role == sbt::ptx::mma::OperandRole::D) emit_line("mov.b32 " + dst_regs[tuple_reg] + ", " + value_word + ";");
    else emit_line("mov.u32 " + dst_regs[tuple_reg] + ", " + value_word + ";");
  }
}

void EmitCtx::emit_materialize_b_tuple_regs(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, const sbt::ptx::mma::BSourceWindowPlan &plan,
                                   int base_reg, const std::vector<std::string> &dst_regs) {
  const auto pack = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::B);
  const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::B);
  require(dst_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));
  require(plan.native_window_n == abi.n, EmitError("invalid.mma.b_slice", func_name, di.pc, mma_detail(di.mma)));

  if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
    for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
      const std::string lo_word = tmp_b32();
      const std::string hi_word = tmp_b32();
      for (uint8_t elem = 0; elem < 2u; ++elem) {
        const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::B, tuple_reg, elem);
        const std::string dst_word = (elem == 0u) ? lo_word : hi_word;
        emit_materialize_b_scalar_value_by_shuffle(di, abi, plan, value, base_reg, dst_word);
        emit_line("and.b32 " + dst_word + ", " + dst_word + ", 0xffff;");
      }
      emit_line("shl.b32 " + hi_word + ", " + hi_word + ", 16;");
      emit_line("or.b32 " + dst_regs[tuple_reg] + ", " + lo_word + ", " + hi_word + ";");
    }
    return;
  }

  for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
    const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::B, tuple_reg, 0u);
    const std::string value_word = tmp_b32();
    emit_materialize_b_scalar_value_by_shuffle(di, abi, plan, value, base_reg, value_word);
    emit_line("mov.u32 " + dst_regs[tuple_reg] + ", " + value_word + ";");
  }
}

void EmitCtx::emit_compute_d_window_coord(const sbt::DecodedInst &di, uint8_t carrier_reg, uint8_t half_elem, bool packed, const std::string &idx_r,
                                 const std::string &row_r, const std::string &col_r) {
  if (packed) {
    emit_line("shl.b32 " + idx_r + ", " + r(0) + ", 1;");
    if (half_elem != 0u) emit_line("add.u32 " + idx_r + ", " + idx_r + ", 1;");
    if (carrier_reg != 0u) emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + std::to_string(static_cast<uint32_t>(carrier_reg) * 64u) + ";");
  } else {
    emit_line("mov.u32 " + idx_r + ", " + r(0) + ";");
    if (carrier_reg != 0u) emit_line("add.u32 " + idx_r + ", " + idx_r + ", " + std::to_string(static_cast<uint32_t>(carrier_reg) * 32u) + ";");
  }

  const uint8_t n = sbt::ptx::mma::shape_n(di.mma);
  require(n == 8u || n == 16u, EmitError("invalid.mma.d_shape", func_name, di.pc, mma_detail(di.mma)));
  emit_line("shr.u32 " + row_r + ", " + idx_r + ", " + std::to_string(n == 8u ? 3u : 4u) + ";");
  emit_line("and.b32 " + col_r + ", " + idx_r + ", " + std::to_string(n - 1u) + ";");
}

void EmitCtx::emit_compute_tuple_candidate_lane_match(const sbt::ptx::mma::ScalarTupleValue &value, const std::string &logical_row_r,
                                             const std::string &logical_col_r, const std::string &lane_r, const std::string &match_p) {
  const std::string row_part = tmp_b32();
  const std::string col_part = tmp_b32();
  const std::string effective_lane = tmp_b32();
  const std::string check_row = tmp_b32();
  const std::string check_col = tmp_b32();
  const std::string lane_valid = tmp_pred();
  const std::string row_match = tmp_pred();
  const std::string col_match = tmp_pred();
  emit_line("sub.u32 " + row_part + ", " + logical_row_r + ", " + std::to_string(value.tile_row_base) + ";");
  if (value.lane_row_shift != 0u) emit_line("shl.b32 " + row_part + ", " + row_part + ", " + std::to_string(value.lane_row_shift) + ";");
  const uint32_t col_bias = static_cast<uint32_t>(value.tile_col_base) + static_cast<uint32_t>(value.lane_col_bias);
  emit_line("sub.u32 " + col_part + ", " + logical_col_r + ", " + std::to_string(col_bias) + ";");
  if (value.lane_col_shift != 0u) emit_line("shr.u32 " + col_part + ", " + col_part + ", " + std::to_string(value.lane_col_shift) + ";");
  emit_line("or.b32 " + effective_lane + ", " + row_part + ", " + col_part + ";");
  emit_line("setp.le.u32 " + lane_valid + ", " + effective_lane + ", 31;");
  if (value.lane_xor_mask == 0u) emit_line("mov.u32 " + lane_r + ", " + effective_lane + ";");
  else emit_line("xor.b32 " + lane_r + ", " + effective_lane + ", " + std::to_string(value.lane_xor_mask) + ";");

  emit_line("shr.u32 " + check_row + ", " + effective_lane + ", " + std::to_string(value.lane_row_shift) + ";");
  if (value.tile_row_base != 0u) emit_line("add.u32 " + check_row + ", " + check_row + ", " + std::to_string(value.tile_row_base) + ";");
  emit_line("and.b32 " + check_col + ", " + effective_lane + ", " + std::to_string(value.lane_col_mask) + ";");
  if (value.lane_col_shift != 0u) emit_line("shl.b32 " + check_col + ", " + check_col + ", " + std::to_string(value.lane_col_shift) + ";");
  if (col_bias != 0u) emit_line("add.u32 " + check_col + ", " + check_col + ", " + std::to_string(col_bias) + ";");
  emit_line("setp.eq.u32 " + row_match + ", " + check_row + ", " + logical_row_r + ";");
  emit_line("setp.eq.u32 " + col_match + ", " + check_col + ", " + logical_col_r + ";");
  emit_line("and.pred " + match_p + ", " + row_match + ", " + col_match + ";");
  emit_line("and.pred " + match_p + ", " + match_p + ", " + lane_valid + ";");
}

void EmitCtx::emit_select_d_scalar_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, const std::string &logical_row_r,
                                     const std::string &native_col_r, const std::vector<std::string> &src_regs,
                                     const std::string &active_p, const std::string &dst_word_r) {
  const auto pack = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D);
  const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::D);
  require(src_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));
  const std::string found = tmp_pred();
  const std::string missing = tmp_pred();
  const std::string guarded_match = tmp_pred();
  emit_line("setp.eq.u32 " + found + ", 1, 0;");
  emit_line("mov.u32 " + dst_word_r + ", 0;");
  for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
    const uint8_t elems = sbt::ptx::mma::packed_values_per_tuple_reg(pack);
    for (uint8_t elem = 0; elem < elems; ++elem) {
      const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::D, tuple_reg, elem);
      const std::string producer_lane = tmp_b32();
      const std::string match = tmp_pred();
      const std::string shuffled = tmp_b32();
      emit_compute_tuple_candidate_lane_match(value, logical_row_r, native_col_r, producer_lane, match);
      if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
        emit_shfl_idx_b32(shuffled, src_regs[tuple_reg], producer_lane);
        if (elem != 0u) emit_line("shr.u32 " + shuffled + ", " + shuffled + ", 16;");
        emit_line("and.b32 " + shuffled + ", " + shuffled + ", 0xffff;");
      } else {
        const std::string bits = tmp_b32();
        emit_line("mov.b32 " + bits + ", " + src_regs[tuple_reg] + ";");
        emit_shfl_idx_b32(shuffled, bits, producer_lane);
      }
      emit_line("and.pred " + guarded_match + ", " + match + ", " + active_p + ";");
      emit_line("@" + guarded_match + " mov.u32 " + dst_word_r + ", " + shuffled + ";");
      emit_line("or.pred " + found + ", " + found + ", " + match + ";");
    }
  }
  emit_line("not.pred " + missing + ", " + found + ";");
  emit_line("and.pred " + missing + ", " + missing + ", " + active_p + ";");
  emit_line("@" + missing + " trap;");
}

void EmitCtx::emit_merge_d_tuple_to_v_window_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset,
                                               const std::vector<std::string> &src_regs) {
  const auto pack = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D);
  const uint8_t cd_carrier_regs =
      (di.mma.cd_type == sbt::MmaCdType::Fp16) ? static_cast<uint8_t>(di.mma.c_regs_per_thread / 2u) : di.mma.c_regs_per_thread;
  for (uint8_t carrier_reg = 0; carrier_reg < cd_carrier_regs; ++carrier_reg) {
    const bool packed = pack == sbt::ptx::mma::PackMode::Packed16x2;
    const uint8_t elems = packed ? 2u : 1u;
    const std::string merged = tmp_b32();
    emit_line("mov.u32 " + merged + ", " + v(di.mma.rd_base + static_cast<int>(carrier_reg)) + ";");
    for (uint8_t elem = 0; elem < elems; ++elem) {
      const std::string idx = tmp_b32();
      const std::string row = tmp_b32();
      const std::string col = tmp_b32();
      const std::string native_col = tmp_b32();
      const std::string selected = tmp_b32();
      const std::string slice_begin_ok = tmp_pred();
      const std::string slice_end_ok = tmp_pred();
      const std::string in_slice = tmp_pred();
      emit_compute_d_window_coord(di, carrier_reg, elem, packed, idx, row, col);
      emit_line("setp.ge.u32 " + slice_begin_ok + ", " + col + ", " + std::to_string(slice_col_offset) + ";");
      emit_line("setp.lt.u32 " + slice_end_ok + ", " + col + ", " + std::to_string(static_cast<uint32_t>(slice_col_offset) + abi.n) + ";");
      emit_line("and.pred " + in_slice + ", " + slice_begin_ok + ", " + slice_end_ok + ";");
      emit_line("sub.u32 " + native_col + ", " + col + ", " + std::to_string(slice_col_offset) + ";");
      emit_select_d_scalar_by_shuffle(di, abi, row, native_col, src_regs, in_slice, selected);
      if (packed) {
        if (elem == 0u) {
          emit_line("@" + in_slice + " and.b32 " + merged + ", " + merged + ", 0xffff0000;");
          emit_line("@" + in_slice + " or.b32 " + merged + ", " + merged + ", " + selected + ";");
        } else {
          const std::string shifted = tmp_b32();
          emit_line("@" + in_slice + " and.b32 " + merged + ", " + merged + ", 0x0000ffff;");
          emit_line("@" + in_slice + " shl.b32 " + shifted + ", " + selected + ", 16;");
          emit_line("@" + in_slice + " or.b32 " + merged + ", " + merged + ", " + shifted + ";");
        }
      } else {
        emit_line("@" + in_slice + " mov.u32 " + merged + ", " + selected + ";");
      }
    }
    emit_line("mov.u32 " + v(di.mma.rd_base + static_cast<int>(carrier_reg)) + ", " + merged + ";");
  }
}

void EmitCtx::validate_native_mma_contract(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi) {
  require(sbt::ptx::mma::shape_m(di.mma) == abi.m, EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
  require(sbt::ptx::mma::shape_k(di.mma) == abi.k, EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
  require(sbt::ptx::mma::shape_n(di.mma) == abi.n, EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
  const uint8_t cd_carrier_regs =
      (di.mma.cd_type == sbt::MmaCdType::Fp16) ? static_cast<uint8_t>(di.mma.c_regs_per_thread / 2u) : di.mma.c_regs_per_thread;
  require(cd_carrier_regs == sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::C),
          EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
}

void EmitCtx::validate_split_n_contract(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi) {
  require(sbt::ptx::mma::shape_m(di.mma) == abi.m, EmitError("invalid.mma.composite_contract", func_name, di.pc, mma_detail(di.mma)));
  require(sbt::ptx::mma::shape_k(di.mma) == abi.k, EmitError("invalid.mma.composite_contract", func_name, di.pc, mma_detail(di.mma)));
  require(sbt::ptx::mma::shape_n(di.mma) == static_cast<uint8_t>(abi.n * 2u),
          EmitError("invalid.mma.composite_contract", func_name, di.pc, mma_detail(di.mma)));
  require(di.mma.b_regs_per_thread == static_cast<uint8_t>(sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::B) * 2u),
          EmitError("invalid.mma.composite_contract", func_name, di.pc, mma_detail(di.mma)));
  const uint8_t cd_carrier_regs =
      (di.mma.cd_type == sbt::MmaCdType::Fp16) ? static_cast<uint8_t>(di.mma.c_regs_per_thread / 2u) : di.mma.c_regs_per_thread;
  require(cd_carrier_regs == static_cast<uint8_t>(sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::C) * 2u),
          EmitError("invalid.mma.composite_contract", func_name, di.pc, mma_detail(di.mma)));
}

void EmitCtx::emit_native_mma_sync(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset) {
  emit_require_full_warp_for_mma(di.pc);

  std::vector<std::string> a_tuple_regs;
  a_tuple_regs.reserve(kMmaATupleRegIds.size());
  for (size_t i = 0; i < kMmaATupleRegIds.size(); ++i) a_tuple_regs.push_back(tmp_b32());

  std::vector<std::string> b_tuple_regs;
  b_tuple_regs.reserve(kMmaBTupleRegIds.size());
  for (size_t i = 0; i < kMmaBTupleRegIds.size(); ++i) b_tuple_regs.push_back(tmp_b32());

  emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::A, 0u, di.mma.rs1_base, di.mma.a_regs_per_thread, a_tuple_regs);

  const auto b_plan = sbt::ptx::mma::b_source_window_plan(di.mma, abi, slice_col_offset);
  emit_materialize_b_tuple_regs(di, abi, b_plan, di.mma.rs2_base + static_cast<int>(b_plan.reg_offset), b_tuple_regs);

  const uint8_t cd_carrier_regs =
      (di.mma.cd_type == sbt::MmaCdType::Fp16) ? static_cast<uint8_t>(di.mma.c_regs_per_thread / 2u) : di.mma.c_regs_per_thread;
  const bool packed_d = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D) == sbt::ptx::mma::PackMode::Packed16x2;
  if (packed_d) {
    const std::string c0 = tmp_b32();
    const std::string c1 = tmp_b32();
    const std::string d0 = tmp_b32();
    const std::string d1 = tmp_b32();
    emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::C, slice_col_offset, di.mma.rd_base, cd_carrier_regs, {c0, c1});
    emit_line(std::string(abi.ptx_opcode) + " {" + d0 + ", " + d1 + "}, {" + a_tuple_regs[0] + ", " + a_tuple_regs[1] + ", " +
              a_tuple_regs[2] + ", " + a_tuple_regs[3] + "}, {" + b_tuple_regs[0] + ", " + b_tuple_regs[1] + "}, {" + c0 + ", " + c1 + "};");
    emit_merge_d_tuple_to_v_window_by_shuffle(di, abi, slice_col_offset, {d0, d1});
  } else {
    const std::string c0 = tmp_f32();
    const std::string c1 = tmp_f32();
    const std::string c2 = tmp_f32();
    const std::string c3 = tmp_f32();
    const std::string d0 = tmp_f32();
    const std::string d1 = tmp_f32();
    const std::string d2 = tmp_f32();
    const std::string d3 = tmp_f32();
    emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::C, slice_col_offset, di.mma.rd_base, cd_carrier_regs, {c0, c1, c2, c3});
    emit_line(std::string(abi.ptx_opcode) + " {" + d0 + ", " + d1 + ", " + d2 + ", " + d3 + "}, {" + a_tuple_regs[0] + ", " +
              a_tuple_regs[1] + ", " + a_tuple_regs[2] + ", " + a_tuple_regs[3] + "}, {" + b_tuple_regs[0] + ", " + b_tuple_regs[1] + "}, {" +
              c0 + ", " + c1 + ", " + c2 + ", " + c3 + "};");
    emit_merge_d_tuple_to_v_window_by_shuffle(di, abi, slice_col_offset, {d0, d1, d2, d3});
  }
}

void EmitCtx::emit_mma_inst(const sbt::DecodedInst &di) {
  require(di.mma.valid, EmitError("invalid.mma.metadata", func_name, di.pc, di.name));
  const std::string detail = mma_detail(di.mma);

  const auto *abi = sbt::ptx::mma::find_abi_desc(di.mma);

  if (di.mma.support_class == sbt::FirstBatchMmaClass::Deferred) {
    throw EmitError("unsupported.mma.deferred", func_name, di.pc, detail);
  }
  if (di.mma.support_class == sbt::FirstBatchMmaClass::Research) {
    throw EmitError("unsupported.mma.research", func_name, di.pc, detail);
  }
  if (di.mma.support_class == sbt::FirstBatchMmaClass::Unsupported || abi == nullptr) {
    throw EmitError("unsupported.mma.family", func_name, di.pc, detail);
  }
  if (di.mma.a_layout != sbt::MmaLayout::Row || di.mma.b_layout != sbt::MmaLayout::Col) {
    throw EmitError("unsupported.mma.layout", func_name, di.pc, detail);
  }

  switch (di.mma.lowering_class) {
  case sbt::MmaLoweringClass::NativeMmaSync:
    validate_native_mma_contract(di, *abi);
    emit_native_mma_sync(di, *abi, 0u);
    return;
  case sbt::MmaLoweringClass::CompositeLowering:
    validate_split_n_contract(di, *abi);
    emit_native_mma_sync(di, *abi, 0u);
    emit_native_mma_sync(di, *abi, abi->n);
    return;
  case sbt::MmaLoweringClass::NativeWmma:
    throw EmitError("unsupported.mma.native_wmma", func_name, di.pc, detail);
  case sbt::MmaLoweringClass::Unsupported:
    throw EmitError("unsupported.mma.lowering", func_name, di.pc, detail);
  }
  throw EmitError("unsupported.mma.lowering", func_name, di.pc, detail);
}

} // namespace sbt::ptx::detail
