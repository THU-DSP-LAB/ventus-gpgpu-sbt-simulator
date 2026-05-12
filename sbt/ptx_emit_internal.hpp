#pragma once

#include "sbt/builtin_semantics.hpp"
#include "sbt/ptx_emit.hpp"
#include "sbt/ptx_mma.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace sbt::ptx::detail {

inline std::string hex8(uint32_t x) {
  std::ostringstream os;
  os << std::hex << std::setfill('0') << std::setw(8) << x;
  return os.str();
}

inline std::string hex_u32(uint32_t x) { return "0x" + hex8(x); }

inline std::string label_bb(uint32_t block_start) { return "BB_" + hex8(block_start); }

inline std::string r(int i) { return "%r" + std::to_string(i); }
inline std::string rd(int i) { return "%rd" + std::to_string(i); }
inline std::string p(int i) { return "%p" + std::to_string(i); }
inline std::string f(int i) { return "%f" + std::to_string(i); }
inline std::string v(int i) { return "%v" + std::to_string(i); }
inline std::string x(int i) { return "%x" + std::to_string(i); }
inline std::string h(int i) { return "%h" + std::to_string(i); }
inline std::string u8(int i) { return "%ub" + std::to_string(i); }
inline std::string u16(int i) { return "%uh" + std::to_string(i); }

static constexpr uint32_t kNumXRegs = 256u;
static constexpr uint32_t kNumVRegs = 256u;
static constexpr uint32_t kBlobWordBytes = 4u;
static constexpr uint32_t kRuntimeEnvBytes = 8u;
static constexpr uint32_t kMachineCtxBytes = 20u;
static constexpr uint32_t kMutableLeaderOffset = 0u;
static constexpr uint32_t kMutableXOffset = kMutableLeaderOffset + kBlobWordBytes;
static constexpr uint32_t kMutableVOffset = kMutableXOffset + kNumXRegs * kBlobWordBytes;
static constexpr uint32_t kMutableStateBytes = kMutableVOffset + kNumVRegs * kBlobWordBytes;
static constexpr uint32_t kRuntimeGlobalOffset = 0u;
static constexpr uint32_t kMachineKnlOffset = 0u;
static constexpr uint32_t kMachinePdsBaseOffset = 4u;
static constexpr uint32_t kMachinePdsSizeOffset = 8u;
static constexpr uint32_t kMachineWarpIdOffset = 12u;
static constexpr uint32_t kMachineWarpsPerBlockOffset = 16u;
static constexpr std::array<int, 4> kMmaATupleRegIds{{3, 4, 5, 6}};
static constexpr std::array<int, 2> kMmaBTupleRegIds{{7, 8}};
static constexpr uint32_t kFullWarpMask = 0xffffffffu;
static constexpr uint32_t kKnlArgBaseOffset = 4u;
static constexpr uint32_t kKnlPrintAddrOffset = 48u;
static constexpr uint32_t kKnlLdsStackSizePerWfOffset = 56u;

enum class VirtualTempKind : size_t {
  B32 = 0,
  B64,
  Pred,
  F32,
  B16,
  U8,
  U16,
  Count,
};

struct VirtualTempSpec final {
  const char *ptx_type;
  const char *name_prefix;
};

static constexpr std::array<VirtualTempSpec, static_cast<size_t>(VirtualTempKind::Count)> kVirtualTempSpecs{{
    {".b32", "%tmp_b32_"},
    {".b64", "%tmp_b64_"},
    {".pred", "%tmp_p_"},
    {".f32", "%tmp_f32_"},
    {".b16", "%tmp_b16_"},
    {".u8", "%tmp_u8_"},
    {".u16", "%tmp_u16_"},
}};

inline bool is_uncond_jump(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::DirectJump && di.rd_class == sbt::RegClass::X &&
         di.rd == 0 && di.imm_kind == sbt::ImmKind::J21;
}

inline bool is_call(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::DirectCall && di.rd_class == sbt::RegClass::X &&
         di.rd != 0 && di.imm_kind == sbt::ImmKind::J21;
}

inline bool is_ret(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::Return && di.rd_class == sbt::RegClass::X &&
         di.rs1_class == sbt::RegClass::X && di.rd == 0 && di.rs1 == 1 && di.imm_kind == sbt::ImmKind::I12 && di.imm == 0;
}

inline void require(bool ok, const EmitError &err) {
  if (!ok) throw err;
}

inline ScalarExecKind scalar_exec_kind_for_inst(const sbt::DecodedInst &di, std::string_view func_name) {
  if (di.scalar_exec_kind != ScalarExecKind::None) return di.scalar_exec_kind;
  throw EmitError("missing.scalar_exec_metadata", std::string(func_name), di.pc, di.name);
}

inline bool is_scalar_branch(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::ScalarBranch;
}

inline bool is_vector_branch(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::VectorBranch;
}

inline bool is_scalar_memory(const sbt::DecodedInst &di, MemAccessKind access) {
  return di.emit.domain == EmitDomain::ScalarMemory && di.emit.mem_access_kind == access;
}

inline bool is_vector_memory(const sbt::DecodedInst &di, MemAccessKind access, MemoryAddrKind addr_kind) {
  return di.emit.domain == EmitDomain::VectorMemory && di.emit.mem_access_kind == access && di.emit.memory_addr_kind == addr_kind;
}

inline bool is_scalar_int(const sbt::DecodedInst &di) { return di.emit.domain == EmitDomain::ScalarInteger; }

inline bool is_scalar_fp(const sbt::DecodedInst &di) { return di.emit.domain == EmitDomain::ScalarFp; }

struct EmitCtx;

bool try_emit_control(EmitCtx &ctx, const sbt::cfg::BundleInst &bi);
bool try_emit_scalar(EmitCtx &ctx, const sbt::DecodedInst &di);
bool try_emit_vector(EmitCtx &ctx, const sbt::DecodedInst &di);
bool try_emit_mma(EmitCtx &ctx, const sbt::DecodedInst &di);
bool try_emit_custom(EmitCtx &ctx, const sbt::DecodedInst &di);

void emit_builtin_call(EmitCtx &ctx, BuiltinKind kind, uint32_t pc_for_err);

void emit_helper_func_signature(std::ostringstream &out, const std::string &ptx_name);

struct ModuleInfo final {
  const std::unordered_map<uint32_t, std::string> *ptx_name_by_addr = nullptr;
};

struct EmitCtx final {
  const sbt::cfg::FunctionCfg &cfg;
  const std::unordered_map<uint32_t, std::string> &sym_by_addr;
  const std::string &func_name;
  const std::string &ptx_name;
  const Options &opt;
  const ModuleInfo &mod;
  const bool is_entry;

  std::ostringstream out;
  std::ostringstream body;
  int tmp_label_id = 0;
  bool emitted_fp_dyn_note = false;
  bool control_protocol_ready = false;
  std::array<uint32_t, static_cast<size_t>(VirtualTempKind::Count)> virtual_temp_counts{};

  EmitCtx(const sbt::cfg::FunctionCfg &cfg_, const std::unordered_map<uint32_t, std::string> &sym_by_addr_,
          const std::string &func_name_, const std::string &ptx_name_, const Options &opt_, const ModuleInfo &mod_, bool is_entry_);

  void require_scalar_exec_kind(const sbt::DecodedInst &di, ScalarExecKind expected, uint32_t pc_for_err) const;

  void require_uniform_pure_scalar(const sbt::DecodedInst &di, uint32_t pc_for_err) const;

  // Fixed register assignment conventions (must match PTX declarations).
  // Only these slots are part of the current machine/runtime/control contract.
  // Lowering scratch must use uniquely allocated %tmp* virtual registers.
  // %r0: laneid
  // %r1: use-point activemask scratch
  // %r2: persistent leader_lane
  // %p0: is_leader
  // %rd0: global_base (global)
  // %rd1: reserved legacy slot (kept stable, not scratch-owned)
  // %rd2: shmem_base (shared)
  // %rd3: reserved legacy slot (kept stable, not scratch-owned)
  // %rd4: reserved legacy slot (kept stable, not scratch-owned)
  // %r26: pds_bitmap_base_vaddr (u32 Ventus numeric address)
  // %r27: pds_pool_num_blocks (u32)
  // %r28: pds_base_vaddr (u32 Ventus numeric address)
  // %r29: pds_size_per_thread (u32 bytes)

  uint32_t x_off(int idx) const { return static_cast<uint32_t>(idx) * 4u; }

  std::string new_label(std::string_view kind) { return "_sbt_" + std::string(kind) + "_" + std::to_string(tmp_label_id++); }

  std::string bb_of_pc(uint32_t pc) const {
    auto it = cfg.inst_pc_to_block.find(pc);
    if (it == cfg.inst_pc_to_block.end()) return "";
    return label_bb(it->second);
  }

  uint32_t block_start_of_pc(uint32_t pc) const {
    auto it = cfg.inst_pc_to_block.find(pc);
    if (it == cfg.inst_pc_to_block.end()) throw EmitError("invalid.cfg", func_name, pc, "pc not mapped to block");
    return it->second;
  }

  std::string alloc_virtual_temp(VirtualTempKind kind) {
    const size_t idx = static_cast<size_t>(kind);
    return std::string(kVirtualTempSpecs[idx].name_prefix) + std::to_string(virtual_temp_counts[idx]++);
  }

  std::string tmp_b32() { return alloc_virtual_temp(VirtualTempKind::B32); }
  std::string tmp_b64() { return alloc_virtual_temp(VirtualTempKind::B64); }
  std::string tmp_pred() { return alloc_virtual_temp(VirtualTempKind::Pred); }
  std::string tmp_f32() { return alloc_virtual_temp(VirtualTempKind::F32); }
  std::string tmp_b16() { return alloc_virtual_temp(VirtualTempKind::B16); }
  std::string tmp_u8() { return alloc_virtual_temp(VirtualTempKind::U8); }
  std::string tmp_u16() { return alloc_virtual_temp(VirtualTempKind::U16); }

  void emit_line(const std::string &s) { body << "  " << s << "\n"; }
  void emit_raw(const std::string &s) { body << s; }
  void emit_label(const std::string &name) { body << name << ":\n"; }

  void emit_fixed_reg_decls();

  void emit_virtual_temp_reg_decls();

  void emit_entry_header();

  void emit_func_header();

  void emit_read_activemask(const std::string &dst_r) { emit_line("activemask.b32 " + dst_r + ";"); }

  void emit_refresh_leader_predicate() { emit_line("setp.eq.u32 " + p(0) + ", " + r(0) + ", " + r(2) + ";"); }

  void emit_select_leader_from_active_mask();

  void emit_warp_sync();

  void emit_trap_if_lane_inactive(uint32_t lane);

  void emit_compute_csr_pds_u32(const std::string &dst_r, bool scalar);

  void emit_builtin_work_group_broadcast(uint32_t dimensions, uint32_t pc_for_err);

  struct AddrMapTemps final {
    std::string addr;
    std::string in_shared_lo;
    std::string in_shared_hi;
    std::string is_shared;
    std::string is_global;
    std::string is_valid;
    std::string offset_u32;
    std::string shared_ptr;
    std::string global_ptr;
  };

  void emit_entry_pds_pool_acquire(uint32_t pc_for_err);

  void emit_entry_pds_pool_release(uint32_t pc_for_err);

  void prepare_control_protocol();

  void emit_boundary_labels_for_block(uint32_t block_start) { (void)block_start; }

  std::string target_label_for_edge(uint32_t src_block, uint32_t dst_block) const;

  void emit_ld_x_u32_leader(const std::string &dst_r, int xreg, uint32_t pc_for_err);

  void emit_broadcast_from_leader(const std::string &dst_r, const std::string &src_r);

  void emit_ld_x_u32_all(const std::string &dst_r, int xreg, uint32_t pc_for_err);

  void emit_st_x_u32_leader(int xreg, const std::string &src_r, uint32_t pc_for_err);

  void emit_st_x_u32_all(int xreg, const std::string &src_r, uint32_t pc_for_err);

  std::string scalar_prefix() const { return ""; }

  void emit_store_param_u32(const std::string &base, uint32_t offset, const std::string &src_r, const std::string &prefix = "");

  void emit_store_param_u64(const std::string &base, uint32_t offset, const std::string &src_rd);

  void emit_load_param_u32(const std::string &dst_r, const std::string &base, uint32_t offset, const std::string &prefix = "");

  void emit_load_param_u64(const std::string &dst_rd, const std::string &base, uint32_t offset);

  void emit_store_mutable_state_blob(const std::string &blob_name);

  void emit_restore_mutable_state_blob(const std::string &blob_name);

  void emit_store_runtime_env_blob(const std::string &blob_name);

  void emit_store_machine_ctx_blob(const std::string &blob_name);

  void emit_load_runtime_env_blob(const std::string &blob_name);

  void emit_load_machine_ctx_blob(const std::string &blob_name);

  void emit_load_knl_u32_scalar(const std::string &dst_r, uint32_t offset, uint32_t pc_for_err);

  void maybe_note_fp_dyn_rm(uint32_t pc_for_err);

  sbt::FpRoundingMode normalize_fp_rm(sbt::FpRoundingMode rm, uint32_t pc_for_err);

  std::string ptx_rm_f32(sbt::FpRoundingMode rm, uint32_t pc_for_err);

  std::string ptx_rm_cvt_i32(sbt::FpRoundingMode rm, uint32_t pc_for_err);

  void emit_ld_x_u32_scalar(const std::string &dst_r, int xreg, uint32_t pc_for_err);

  void emit_st_x_u32_scalar(int xreg, const std::string &src_r, uint32_t pc_for_err);

  std::string emit_ld_x_u32_all_tmp(int xreg, uint32_t pc_for_err);

  std::string emit_ld_x_u32_scalar_tmp(int xreg, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_st_u32_scalar(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u8_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u16_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_st_u8_scalar(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err);

  void emit_addr_map_and_st_u16_scalar(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err);

  AddrMapTemps emit_prepare_addr_mapping(const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_st_u32(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err);

  void emit_addr_map_and_st_u32_leader(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u8_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u16_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u8_zext_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_ld_u16_zext_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err);

  void emit_addr_map_and_st_u8(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err);

  void emit_addr_map_and_st_u16(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err);

  void emit_addr_map_and_st_u8_leader(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err);

  void emit_addr_map_and_st_u16_leader(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err);

  void emit_builtin_get_id(std::string_view kind, uint32_t pc_for_err);

  void emit_builtin_get_global_size(uint32_t pc_for_err);

  void emit_builtin_fmaxff(uint32_t pc_for_err);

  void emit_builtin_sqrtf(uint32_t pc_for_err);

  void emit_builtin_unary_f32_inplace(std::string_view opname, const std::string &dst_v, uint32_t pc_for_err);

  void emit_builtin_vec4_cos(uint32_t pc_for_err);

  void emit_builtin_vec4_sin(uint32_t pc_for_err);

  void emit_builtin_vec4_sqrt(uint32_t pc_for_err);

  void emit_builtin_vec4_fabs(uint32_t pc_for_err);

  void emit_builtin_vec4_tan(uint32_t pc_for_err);

  void emit_builtin_mad24iii(uint32_t pc_for_err);

  void emit_direct_call(const std::string &callee_ptx);

  void emit_scalar_fclass_s(const sbt::DecodedInst &di, uint32_t pc_for_err);

  bool try_emit_scalar_fp(const sbt::DecodedInst &di);

  std::string mma_detail(const sbt::MmaInstInfo &mma) const;

  void emit_require_full_warp_for_mma(uint32_t pc_for_err);

  void emit_shfl_idx_b32(const std::string &dst_r, const std::string &src_r, const std::string &lane_r);

  void emit_select_u32_by_index(const std::string &dst_r, const std::vector<std::string> &candidates, const std::string &idx_r);

  void emit_gather_v_window_word_by_shuffle(int base_reg, uint8_t reg_count, const std::string &source_reg_r,
                                            const std::string &source_lane_r, const std::string &dst_r);

  void emit_compute_tuple_logical_coord(const sbt::ptx::mma::ScalarTupleValue &value, const std::string &row_r, const std::string &col_r);

  void emit_compute_window_index_from_logical_coord(const sbt::DecodedInst &di, sbt::ptx::mma::OperandRole role, uint8_t slice_col_offset,
                                                    const std::string &logical_row_r, const std::string &logical_col_r,
                                                    const std::string &idx_r, const std::string &tmp_r);

  void emit_compute_b_window_index_from_logical_coord(const sbt::DecodedInst &di, const sbt::ptx::mma::BSourceWindowPlan &plan,
                                                      const std::string &logical_n_r, const std::string &logical_k_r, const std::string &idx_r,
                                                      const std::string &tmp_r);

  void emit_materialize_scalar_value_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi,
                                                sbt::ptx::mma::OperandRole role, uint8_t slice_col_offset,
                                                const sbt::ptx::mma::ScalarTupleValue &value, int base_reg, uint8_t window_reg_count,
                                                const std::string &dst_r);

  void emit_materialize_b_scalar_value_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi,
                                                  const sbt::ptx::mma::BSourceWindowPlan &plan,
                                                  const sbt::ptx::mma::ScalarTupleValue &value, int base_reg,
                                                  const std::string &dst_r);

  void emit_materialize_tuple_regs(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, sbt::ptx::mma::OperandRole role,
                                   uint8_t slice_col_offset, int base_reg, uint8_t window_reg_count, const std::vector<std::string> &dst_regs);

  void emit_materialize_b_tuple_regs(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, const sbt::ptx::mma::BSourceWindowPlan &plan,
                                     int base_reg, const std::vector<std::string> &dst_regs);

  void emit_compute_d_window_coord(const sbt::DecodedInst &di, uint8_t carrier_reg, uint8_t half_elem, bool packed, const std::string &idx_r,
                                   const std::string &row_r, const std::string &col_r);

  void emit_compute_tuple_candidate_lane_match(const sbt::ptx::mma::ScalarTupleValue &value, const std::string &logical_row_r,
                                               const std::string &logical_col_r, const std::string &lane_r, const std::string &match_p);

  void emit_select_d_scalar_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, const std::string &logical_row_r,
                                       const std::string &native_col_r, const std::vector<std::string> &src_regs,
                                       const std::string &active_p, const std::string &dst_word_r);

  void emit_merge_d_tuple_to_v_window_by_shuffle(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset,
                                                 const std::vector<std::string> &src_regs);

  void validate_native_mma_contract(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi);

  void validate_split_n_contract(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi);

  void emit_native_mma_sync(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset);

  void emit_mma_inst(const sbt::DecodedInst &di);

  void emit_comment_if_needed(const sbt::cfg::BundleInst &bi);

  void validate_shared_preconditions(const sbt::DecodedInst &di) const;

  void emit_one_inst(const sbt::cfg::BundleInst &bi);

  void emit_prologue();

  void emit_func_prologue();

  void emit_fallthrough_edge_if_needed(const sbt::cfg::BasicBlock &bb);

  void emit_body();
};

} // namespace sbt::ptx::detail
