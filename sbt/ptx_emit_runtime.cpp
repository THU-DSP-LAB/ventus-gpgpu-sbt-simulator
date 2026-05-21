#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

void EmitCtx::emit_select_leader_from_active_mask() {
  emit_read_activemask(r(1));
  emit_line("bfind.u32 " + r(2) + ", " + r(1) + ";");
  emit_refresh_leader_predicate();
}

void EmitCtx::emit_warp_sync() {
  emit_read_activemask(r(1));
  emit_line("bar.warp.sync " + r(1) + ";");
}

void EmitCtx::emit_trap_if_lane_inactive(uint32_t lane) {
  const std::string active_mask = tmp_b32();
  const std::string lane_mask = tmp_b32();
  const std::string inactive = tmp_pred();
  require(lane < 32u, EmitError("unsupported.fixed_lane", func_name, cfg.start, "lane>=32"));
  emit_read_activemask(active_mask);
  emit_line("and.b32 " + lane_mask + ", " + active_mask + ", " + std::to_string(1u << lane) + ";");
  emit_line("setp.eq.u32 " + inactive + ", " + lane_mask + ", 0;");
  emit_line("@" + inactive + " trap;");
}

void EmitCtx::emit_compute_csr_pds_u32(const std::string &dst_r, bool scalar) {
  const std::string pre = scalar ? scalar_prefix() : "";
  emit_line(pre + "ld.shared.u32 " + dst_r + ", [__sbt_pds_wg_base];");
}

void EmitCtx::emit_entry_pds_pool_acquire(uint32_t pc_for_err) {
  const std::string L_thread0_done = new_label("pds_acquire_t0_done");
  const std::string L_skip_thread0 = new_label("pds_acquire_skip_t0");
  const std::string L_scan_word = new_label("pds_scan_word");
  const std::string L_try_word = new_label("pds_try_word");
  const std::string L_alloc_success = new_label("pds_alloc_success");
  const std::string L_scan_restart = new_label("pds_scan_restart");

  emit_line("setp.eq.u32 " + p(6) + ", " + r(9) + ", 0;");
  emit_line("@!" + p(6) + " bra " + L_skip_thread0 + ";");
  emit_line("st.shared.u32 [__sbt_pds_exit_count], 0;");

  emit_line("setp.eq.u32 " + p(7) + ", " + r(29) + ", 0;");
  emit_line("@" + p(7) + " st.shared.u32 [__sbt_pds_block_idx], 0;");
  emit_line("@" + p(7) + " st.shared.u32 [__sbt_pds_wg_base], " + r(28) + ";");
  emit_line("@" + p(7) + " bra " + L_thread0_done + ";");

  emit_line("setp.eq.u32 " + p(7) + ", " + r(27) + ", 0;");
  emit_line("@" + p(7) + " trap;");
  emit_line("setp.lt.u32 " + p(7) + ", " + r(26) + ", " + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("@" + p(7) + " trap;");

  emit_line("add.u32 " + r(23) + ", " + r(27) + ", 31;");
  emit_line("shr.u32 " + r(23) + ", " + r(23) + ", 5;"); // bitmap word count
  emit_line("mov.u32 " + r(21) + ", 0;"); // word index

  emit_label(L_scan_word);
  emit_line("setp.ge.u32 " + p(7) + ", " + r(21) + ", " + r(23) + ";");
  emit_line("@" + p(7) + " bra " + L_scan_restart + ";");

  emit_line("shl.b32 " + r(22) + ", " + r(21) + ", 2;");
  emit_line("add.u32 " + r(24) + ", " + r(26) + ", " + r(22) + ";"); // bitmap word numeric addr
  emit_line("add.u32 " + r(25) + ", " + r(24) + ", -" + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("cvt.u64.u32 " + rd(18) + ", " + r(25) + ";");
  emit_line("add.u64 " + rd(18) + ", " + rd(0) + ", " + rd(18) + ";");
  emit_line("ld.global.u32 " + r(25) + ", [" + rd(18) + "];");
  emit_line("not.b32 " + r(22) + ", " + r(25) + ";"); // free bits

  // Mask out out-of-range bits in the last bitmap word.
  emit_line("mul.lo.u32 " + r(24) + ", " + r(21) + ", 32;");
  emit_line("sub.u32 " + r(24) + ", " + r(27) + ", " + r(24) + ";"); // remaining blocks in this word
  emit_line("setp.ge.u32 " + p(7) + ", " + r(24) + ", 32;");
  emit_line("@" + p(7) + " mov.u32 " + r(20) + ", 0xffffffff;");
  emit_line("@!" + p(7) + " mov.u32 " + r(20) + ", 1;");
  emit_line("@!" + p(7) + " shl.b32 " + r(20) + ", " + r(20) + ", " + r(24) + ";");
  emit_line("@!" + p(7) + " add.u32 " + r(20) + ", " + r(20) + ", -1;");
  emit_line("and.b32 " + r(22) + ", " + r(22) + ", " + r(20) + ";");

  emit_line("setp.eq.u32 " + p(7) + ", " + r(22) + ", 0;");
  emit_line("@" + p(7) + " add.u32 " + r(21) + ", " + r(21) + ", 1;");
  emit_line("@" + p(7) + " bra " + L_scan_word + ";");

  emit_label(L_try_word);
  emit_line("bfind.u32 " + r(20) + ", " + r(22) + ";");
  emit_line("mov.u32 " + r(19) + ", 1;");
  emit_line("shl.b32 " + r(19) + ", " + r(19) + ", " + r(20) + ";");
  emit_line("atom.global.or.b32 " + r(18) + ", [" + rd(18) + "], " + r(19) + ";");
  emit_line("and.b32 " + r(15) + ", " + r(18) + ", " + r(19) + ";");
  emit_line("setp.eq.u32 " + p(7) + ", " + r(15) + ", 0;");
  emit_line("@" + p(7) + " bra " + L_alloc_success + ";");
  emit_line("xor.b32 " + r(22) + ", " + r(22) + ", " + r(19) + ";");
  emit_line("setp.ne.u32 " + p(8) + ", " + r(22) + ", 0;");
  emit_line("@" + p(8) + " bra " + L_try_word + ";");
  emit_line("add.u32 " + r(21) + ", " + r(21) + ", 1;");
  emit_line("bra " + L_scan_word + ";");

  emit_label(L_scan_restart);
  emit_line("mov.u32 " + r(21) + ", 0;");
  emit_line("bra " + L_scan_word + ";");

  emit_label(L_alloc_success);
  emit_line("mad.lo.u32 " + r(18) + ", " + r(21) + ", 32, " + r(20) + ";"); // block index
  emit_line("st.shared.u32 [__sbt_pds_block_idx], " + r(18) + ";");
  emit_line("shl.b32 " + r(15) + ", " + r(29) + ", 5;"); // bytes per wf
  emit_line("mul.lo.u32 " + r(16) + ", " + r(12) + ", " + r(15) + ";"); // bytes per wg
  emit_line("cvt.u64.u32 " + rd(16) + ", " + r(18) + ";");
  emit_line("cvt.u64.u32 " + rd(15) + ", " + r(16) + ";");
  emit_line("mul.lo.u64 " + rd(16) + ", " + rd(16) + ", " + rd(15) + ";");
  emit_line("cvt.u64.u32 " + rd(15) + ", " + r(28) + ";");
  emit_line("add.u64 " + rd(16) + ", " + rd(16) + ", " + rd(15) + ";");
  emit_line("cvt.u32.u64 " + r(16) + ", " + rd(16) + ";");
  emit_line("st.shared.u32 [__sbt_pds_wg_base], " + r(16) + ";");

  emit_label(L_thread0_done);
  emit_label(L_skip_thread0);
  emit_line("bar.sync 0;");
  (void)pc_for_err;
}

void EmitCtx::emit_entry_pds_pool_release(uint32_t pc_for_err) {
  const std::string L_release_done = new_label("pds_release_done");

  // Divergence-safe release:
  // Every exiting thread does one atomic increment in shared memory.
  // Only the last exiting thread releases the bitmap slot.
  emit_line("mov.u32 " + r(21) + ", %ntid.x;");
  emit_line("mov.u32 " + r(22) + ", %ntid.y;");
  emit_line("mov.u32 " + r(23) + ", %ntid.z;");
  emit_line("mul.lo.u32 " + r(21) + ", " + r(21) + ", " + r(22) + ";");
  emit_line("mul.lo.u32 " + r(21) + ", " + r(21) + ", " + r(23) + ";"); // block thread count
  emit_line("atom.shared.add.u32 " + r(17) + ", [__sbt_pds_exit_count], 1;");
  emit_line("add.u32 " + r(17) + ", " + r(17) + ", 1;");
  emit_line("setp.ne.u32 " + p(8) + ", " + r(17) + ", " + r(21) + ";");
  emit_line("@" + p(8) + " bra " + L_release_done + ";");

  emit_line("setp.eq.u32 " + p(7) + ", " + r(29) + ", 0;");
  emit_line("@" + p(7) + " bra " + L_release_done + ";");

  emit_line("ld.shared.u32 " + r(18) + ", [__sbt_pds_block_idx];");
  emit_line("shr.u32 " + r(21) + ", " + r(18) + ", 5;");
  emit_line("and.b32 " + r(20) + ", " + r(18) + ", 31;");
  emit_line("mov.u32 " + r(19) + ", 1;");
  emit_line("shl.b32 " + r(19) + ", " + r(19) + ", " + r(20) + ";");
  emit_line("not.b32 " + r(19) + ", " + r(19) + ";");

  emit_line("shl.b32 " + r(22) + ", " + r(21) + ", 2;");
  emit_line("add.u32 " + r(24) + ", " + r(26) + ", " + r(22) + ";");
  emit_line("add.u32 " + r(25) + ", " + r(24) + ", -" + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("cvt.u64.u32 " + rd(18) + ", " + r(25) + ";");
  emit_line("add.u64 " + rd(18) + ", " + rd(0) + ", " + rd(18) + ";");
  emit_line("atom.global.and.b32 " + r(17) + ", [" + rd(18) + "], " + r(19) + ";");

  emit_label(L_release_done);
  (void)pc_for_err;
}

void EmitCtx::emit_load_knl_u32_scalar(const std::string &dst_r, uint32_t offset, uint32_t pc_for_err) {
  emit_line("add.u32 " + r(16) + ", " + r(30) + ", " + std::to_string(offset) + ";");
  emit_addr_map_and_ld_u32_scalar(dst_r, r(16), pc_for_err);
}

void EmitCtx::emit_prologue() {
  // Params:
  //  - global_base: backing buffer for [global_base_vaddr, 0x1_0000_0000)
  //  - knl_vaddr: Ventus numeric address of metadata buffer (u32)
  //  - pds_base_vaddr: Ventus numeric address of the global PDS buffer base (u32)
  //  - pds_size_per_thread: bytes of private memory per thread (u32)
  //  - pds_bitmap_base_vaddr: Ventus numeric address of PDS allocation bitmap (u32)
  //  - pds_pool_num_blocks: number of reusable WG blocks in PDS pool (u32)
  // Load params and compute global/shared base pointers.
  emit_line("ld.param.u64 " + rd(10) + ", [global_base];");
  emit_line("cvta.to.global.u64 " + rd(0) + ", " + rd(10) + ";");
  emit_line("ld.param.u32 " + r(30) + ", [knl_vaddr];");
  emit_line("ld.param.u32 " + r(28) + ", [pds_base_vaddr];");
  emit_line("ld.param.u32 " + r(29) + ", [pds_size_per_thread];");
  emit_line("ld.param.u32 " + r(26) + ", [pds_bitmap_base_vaddr];");
  emit_line("ld.param.u32 " + r(27) + ", [pds_pool_num_blocks];");
  emit_load_knl_u32_scalar(r(17), kKnlLdsStackSizePerWfOffset, /*pc_for_err=*/cfg.start);

  // lane id (0..31)
  emit_line("mov.u32 " + r(0) + ", %laneid;");

  // thread linear id = tid.x + ntid.x*(tid.y + ntid.y*tid.z)
  emit_line("mov.u32 " + r(3) + ", %tid.x;");
  emit_line("mov.u32 " + r(4) + ", %tid.y;");
  emit_line("mov.u32 " + r(5) + ", %tid.z;");
  emit_line("mov.u32 " + r(6) + ", %ntid.x;");
  emit_line("mov.u32 " + r(7) + ", %ntid.y;");
  emit_line("mov.u32 " + r(8) + ", %ntid.z;");
  emit_line("mul.lo.u32 " + r(9) + ", " + r(7) + ", " + r(5) + ";");
  emit_line("add.u32 " + r(9) + ", " + r(9) + ", " + r(4) + ";");
  emit_line("mul.lo.u32 " + r(9) + ", " + r(9) + ", " + r(6) + ";");
  emit_line("add.u32 " + r(9) + ", " + r(9) + ", " + r(3) + ";");

  // warp_id_in_block = linear >> 5
  emit_line("shr.u32 " + r(10) + ", " + r(9) + ", 5;");

  // warps_per_block = (threads + 31) >> 5
  emit_line("mul.lo.u32 " + r(11) + ", " + r(6) + ", " + r(7) + ";");
  emit_line("mul.lo.u32 " + r(11) + ", " + r(11) + ", " + r(8) + ";");
  emit_line("add.u32 " + r(12) + ", " + r(11) + ", 31;");
  emit_line("shr.u32 " + r(12) + ", " + r(12) + ", 5;");

  // dynamic shared base
  // NOTE: for `.extern .shared` symbols (dynamic shared), the symbol address is already in the shared state-space.
  // Using `cvta.to.shared` here can produce an address that tools/runtime treat as out-of-bounds.
  emit_line("mov.u64 " + rd(2) + ", __sbt_shmem;");

  // Reserved legacy shared scalar backing slot (kept only to avoid register-map churn).
  emit_line("mul.lo.u32 " + r(13) + ", " + r(10) + ", " + r(17) + ";");
  emit_line("cvt.u64.u32 " + rd(13) + ", " + r(13) + ";");
  emit_line("add.u64 " + rd(3) + ", " + rd(2) + ", " + rd(13) + ";");

  emit_line("mov.u64 " + rd(3) + ", 0;");
  emit_select_leader_from_active_mask();

  // Fail fast if pds_base_vaddr is outside the supported Global numeric address range.
  // Allow pds_size_per_thread==0 to bypass the check (some kernels may not allocate private memory).
  emit_line("setp.lt.u32 " + p(1) + ", " + r(28) + ", " + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("setp.ne.u32 " + p(2) + ", " + r(29) + ", 0;");
  emit_line("and.pred " + p(1) + ", " + p(1) + ", " + p(2) + ";");
  emit_line("@" + p(1) + " trap;");
  emit_entry_pds_pool_acquire(cfg.start);

  // Match `_start` ABI: tp (x4) starts at 0 and is used as a spill-stack cursor.
  emit_line("mov.u32 " + r(15) + ", 0;");
  emit_st_x_u32_scalar(/*x4=*/4, r(15), /*pc_for_err=*/cfg.start);

  if (opt.global_pointer_vaddr != 0) {
    emit_line("mov.u32 " + r(15) + ", " + hex_u32(opt.global_pointer_vaddr) + ";");
    emit_st_x_u32_scalar(/*x3=*/3, r(15), /*pc_for_err=*/cfg.start);
  }

  emit_load_knl_u32_scalar(r(17), kKnlLdsStackSizePerWfOffset, /*pc_for_err=*/cfg.start);
  emit_load_knl_u32_scalar(r(18), kKnlLdsNonStackSizeOffset, /*pc_for_err=*/cfg.start);

  // x2 = shared_base + ldsNonStackSize + warp_id * ldsStackSizePerWf
  emit_line("mul.lo.u32 " + r(15) + ", " + r(10) + ", " + r(17) + ";");
  emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + r(18) + ";");
  emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
  emit_st_x_u32_scalar(/*x2=*/2, r(15), /*pc_for_err=*/cfg.start);

  // Match `_start` ABI: s0 (x8) points past the per-workgroup LDS stack region:
  //   s0 = CSR_LDS + ldsNonStackSize + CSR_NUMW*ldsStackSizePerWf
  // In this backend `shared_base_vaddr` models the CSR_LDS numeric base, and `warps_per_block` models CSR_NUMW.
  // Note: kernels may further adjust s0 in their own prologue (e.g. `addi s0, s0, <frame_bytes>`). We treat that
  // as frame allocation and do not attempt to compensate it here.
  emit_line("mul.lo.u32 " + r(15) + ", " + r(12) + ", " + r(17) + ";");
  emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + r(18) + ";");
  emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
  emit_st_x_u32_scalar(/*x8=*/8, r(15), /*pc_for_err=*/cfg.start);

  // x10 (a0) is the first argument register. PoCL Ventus kernels expect:
  //   a0 = *(u32*)(CSR_KNL + 4)  (arg buffer base)
  // because the original `_start` loads it from the hardware metadata buffer before jumping to the kernel entry.
  emit_load_knl_u32_scalar(r(17), kKnlArgBaseOffset, /*pc_for_err=*/cfg.start);
  emit_st_x_u32_scalar(/*x10=*/10, r(17), /*pc_for_err=*/cfg.start);

  emit_warp_sync();

  // Fallthrough to entry BB.
}

void EmitCtx::emit_func_prologue() {
  // Pass-through params computed in the caller and required for address mapping / CSR reads.
  emit_line("mov.u32 " + r(0) + ", %laneid;");
  emit_load_runtime_env_blob("__sbt_runtime_env_in");
  emit_load_machine_ctx_blob("__sbt_machine_ctx_in");
  emit_line("mov.u64 " + rd(2) + ", __sbt_shmem;");
  emit_line("mov.u64 " + rd(3) + ", 0;");
  emit_restore_mutable_state_blob("__sbt_mutable_state_in");
}

} // namespace sbt::ptx::detail
