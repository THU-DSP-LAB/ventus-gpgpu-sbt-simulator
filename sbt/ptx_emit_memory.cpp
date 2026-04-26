#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

void EmitCtx::emit_ld_x_u32_leader(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
  if (xreg == 0) {
    emit_line("@" + p(0) + " mov.u32 " + dst_r + ", 0;");
    return;
  }
  require(xreg >= 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<0"));
  emit_line("@" + p(0) + " mov.u32 " + dst_r + ", " + x(xreg) + ";");
}

void EmitCtx::emit_broadcast_from_leader(const std::string &dst_r, const std::string &src_r) {
  emit_read_activemask(r(1));
  emit_line("shfl.sync.idx.b32 " + dst_r + ", " + src_r + ", " + r(2) + ", 0x1f, " + r(1) + ";");
}

void EmitCtx::emit_ld_x_u32_all(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
  if (xreg == 0) {
    emit_line("mov.u32 " + dst_r + ", 0;");
    return;
  }
  require(xreg >= 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<0"));
  emit_line("mov.u32 " + dst_r + ", " + x(xreg) + ";");
}

void EmitCtx::emit_st_x_u32_leader(int xreg, const std::string &src_r, uint32_t pc_for_err) {
  if (xreg == 0) return; // x0 is hard-wired zero.
  require(xreg > 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<=0"));
  emit_line("@" + p(0) + " mov.u32 " + x(xreg) + ", " + src_r + ";");
}

void EmitCtx::emit_st_x_u32_all(int xreg, const std::string &src_r, uint32_t pc_for_err) {
  if (xreg == 0) return; // x0 is hard-wired zero.
  require(xreg > 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<=0"));
  emit_line("mov.u32 " + x(xreg) + ", " + src_r + ";");
}

void EmitCtx::emit_ld_x_u32_scalar(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
  emit_ld_x_u32_all(dst_r, xreg, pc_for_err);
}

void EmitCtx::emit_st_x_u32_scalar(int xreg, const std::string &src_r, uint32_t pc_for_err) {
  emit_st_x_u32_all(xreg, src_r, pc_for_err);
}

std::string EmitCtx::emit_ld_x_u32_all_tmp(int xreg, uint32_t pc_for_err) {
  const std::string dst = tmp_b32();
  emit_ld_x_u32_all(dst, xreg, pc_for_err);
  return dst;
}

std::string EmitCtx::emit_ld_x_u32_scalar_tmp(int xreg, uint32_t pc_for_err) {
  const std::string dst = tmp_b32();
  emit_ld_x_u32_scalar(dst, xreg, pc_for_err);
  return dst;
}

void EmitCtx::emit_addr_map_and_ld_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
}

void EmitCtx::emit_addr_map_and_st_u32_scalar(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
  emit_select_leader_from_active_mask();
  emit_addr_map_and_st_u32_leader(addr_r, src_r, pc_for_err);
  emit_warp_sync();
}

void EmitCtx::emit_addr_map_and_ld_u8_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  emit_addr_map_and_ld_u8_zext_u32(dst_r, addr_r, pc_for_err);
}

void EmitCtx::emit_addr_map_and_ld_u16_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  emit_addr_map_and_ld_u16_zext_u32(dst_r, addr_r, pc_for_err);
}

void EmitCtx::emit_addr_map_and_st_u8_scalar(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
  emit_select_leader_from_active_mask();
  emit_addr_map_and_st_u8_leader(addr_r, src_u8, pc_for_err);
  emit_warp_sync();
}

void EmitCtx::emit_addr_map_and_st_u16_scalar(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
  emit_select_leader_from_active_mask();
  emit_addr_map_and_st_u16_leader(addr_r, src_u16, pc_for_err);
  emit_warp_sync();
}

EmitCtx::AddrMapTemps EmitCtx::emit_prepare_addr_mapping(const std::string &addr_r, uint32_t pc_for_err) {
  (void)pc_for_err;
  AddrMapTemps temps{
      .addr = tmp_b32(),
      .in_shared_lo = tmp_pred(),
      .in_shared_hi = tmp_pred(),
      .is_shared = tmp_pred(),
      .is_global = tmp_pred(),
      .is_valid = tmp_pred(),
      .offset_u32 = tmp_b32(),
      .shared_ptr = tmp_b64(),
      .global_ptr = tmp_b64(),
  };

  emit_line("mov.u32 " + temps.addr + ", " + addr_r + ";");

  // Shared addresses live in [shared_base_vaddr, global_base_vaddr); everything below shared is invalid.
  emit_line("setp.ge.u32 " + temps.in_shared_lo + ", " + temps.addr + ", " + hex_u32(opt.shared_base_vaddr) + ";");
  emit_line("setp.lt.u32 " + temps.in_shared_hi + ", " + temps.addr + ", " + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("and.pred " + temps.is_shared + ", " + temps.in_shared_lo + ", " + temps.in_shared_hi + ";");
  emit_line("setp.ge.u32 " + temps.is_global + ", " + temps.addr + ", " + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("or.pred " + temps.is_valid + ", " + temps.is_shared + ", " + temps.is_global + ";");
  emit_line("@!" + temps.is_valid + " trap;");

  emit_line("add.u32 " + temps.offset_u32 + ", " + temps.addr + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
  emit_line("cvt.u64.u32 " + temps.shared_ptr + ", " + temps.offset_u32 + ";");
  emit_line("add.u64 " + temps.shared_ptr + ", " + rd(2) + ", " + temps.shared_ptr + ";");

  emit_line("add.u32 " + temps.offset_u32 + ", " + temps.addr + ", -" + hex_u32(opt.global_base_vaddr) + ";");
  emit_line("cvt.u64.u32 " + temps.global_ptr + ", " + temps.offset_u32 + ";");
  emit_line("add.u64 " + temps.global_ptr + ", " + rd(0) + ", " + temps.global_ptr + ";");
  return temps;
}

void EmitCtx::emit_addr_map_and_ld_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
  emit_line("@" + temps.is_shared + " ld.shared.u32 " + dst_r + ", [" + temps.shared_ptr + "];");
  emit_line("@" + temps.is_global + " ld.global.u32 " + dst_r + ", [" + temps.global_ptr + "];");
}

void EmitCtx::emit_addr_map_and_ld_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  const std::string L_after = new_label("leader_ld32_after");
  emit_line("@!" + p(0) + " bra " + L_after + ";");
  emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
  emit_label(L_after);
}

void EmitCtx::emit_addr_map_and_st_u32(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
  const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
  emit_line("@" + temps.is_shared + " st.shared.u32 [" + temps.shared_ptr + "], " + src_r + ";");
  emit_line("@" + temps.is_global + " st.global.u32 [" + temps.global_ptr + "], " + src_r + ";");
}

void EmitCtx::emit_addr_map_and_st_u32_leader(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
  const std::string L_after = new_label("leader_st32_after");
  emit_line("@!" + p(0) + " bra " + L_after + ";");
  emit_addr_map_and_st_u32(addr_r, src_r, pc_for_err);
  emit_label(L_after);
}

void EmitCtx::emit_addr_map_and_ld_u8_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
  const std::string byte_val = tmp_u8();
  emit_line("@" + temps.is_shared + " ld.shared.u8 " + byte_val + ", [" + temps.shared_ptr + "];");
  emit_line("@" + temps.is_global + " ld.global.u8 " + byte_val + ", [" + temps.global_ptr + "];");
  emit_line("cvt.u32.u8 " + dst_r + ", " + byte_val + ";");
}

void EmitCtx::emit_addr_map_and_ld_u16_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
  const std::string half_val = tmp_u16();
  emit_line("@" + temps.is_shared + " ld.shared.u16 " + half_val + ", [" + temps.shared_ptr + "];");
  emit_line("@" + temps.is_global + " ld.global.u16 " + half_val + ", [" + temps.global_ptr + "];");
  emit_line("cvt.u32.u16 " + dst_r + ", " + half_val + ";");
}

void EmitCtx::emit_addr_map_and_ld_u8_zext_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  const std::string L_after = new_label("leader_ldu8_after");
  emit_line("@!" + p(0) + " bra " + L_after + ";");
  emit_addr_map_and_ld_u8_zext_u32(dst_r, addr_r, pc_for_err);
  emit_label(L_after);
}

void EmitCtx::emit_addr_map_and_ld_u16_zext_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
  const std::string L_after = new_label("leader_ldu16_after");
  emit_line("@!" + p(0) + " bra " + L_after + ";");
  emit_addr_map_and_ld_u16_zext_u32(dst_r, addr_r, pc_for_err);
  emit_label(L_after);
}

void EmitCtx::emit_addr_map_and_st_u8(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
  const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
  emit_line("@" + temps.is_shared + " st.shared.u8 [" + temps.shared_ptr + "], " + src_u8 + ";");
  emit_line("@" + temps.is_global + " st.global.u8 [" + temps.global_ptr + "], " + src_u8 + ";");
}

void EmitCtx::emit_addr_map_and_st_u16(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
  const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
  emit_line("@" + temps.is_shared + " st.shared.u16 [" + temps.shared_ptr + "], " + src_u16 + ";");
  emit_line("@" + temps.is_global + " st.global.u16 [" + temps.global_ptr + "], " + src_u16 + ";");
}

void EmitCtx::emit_addr_map_and_st_u8_leader(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
  const std::string L_after = new_label("leader_stu8_after");
  emit_line("@!" + p(0) + " bra " + L_after + ";");
  emit_addr_map_and_st_u8(addr_r, src_u8, pc_for_err);
  emit_label(L_after);
}

void EmitCtx::emit_addr_map_and_st_u16_leader(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
  const std::string L_after = new_label("leader_stu16_after");
  emit_line("@!" + p(0) + " bra " + L_after + ";");
  emit_addr_map_and_st_u16(addr_r, src_u16, pc_for_err);
  emit_label(L_after);
}

} // namespace sbt::ptx::detail
