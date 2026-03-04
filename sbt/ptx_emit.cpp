#include "sbt/ptx_emit.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace sbt::ptx {
namespace {

static std::string hex8(uint32_t x) {
  std::ostringstream os;
  os << std::hex << std::setfill('0') << std::setw(8) << x;
  return os.str();
}

static std::string hex_u32(uint32_t x) { return "0x" + hex8(x); }

static std::string label_bb(uint32_t block_start) { return "BB_" + hex8(block_start); }

static std::string r(int i) { return "%r" + std::to_string(i); }
static std::string rd(int i) { return "%rd" + std::to_string(i); }
static std::string p(int i) { return "%p" + std::to_string(i); }
static std::string f(int i) { return "%f" + std::to_string(i); }
static std::string v(int i) { return "%v" + std::to_string(i); }
static std::string u8(int i) { return "%ub" + std::to_string(i); }
static std::string u16(int i) { return "%uh" + std::to_string(i); }

static bool is_scalar_branch(std::string_view name) {
  return name == "beq" || name == "bne" || name == "blt" || name == "bge" || name == "bltu" || name == "bgeu";
}

static bool is_vector_branch(std::string_view name) {
  return name == "vbeq" || name == "vbne" || name == "vblt" || name == "vbge" || name == "vbltu" || name == "vbgeu";
}

static bool is_vector_load(std::string_view name) {
  return name == "vlw12_v" || name == "vlb12_v" || name == "vlbu12_v" || name == "vlh12_v" || name == "vlhu12_v" || name == "vlw_v";
}

static bool is_vector_store(std::string_view name) { return name == "vsw12_v" || name == "vsb12_v" || name == "vsh12_v" || name == "vsw_v"; }

static bool is_scalar_load(std::string_view name) {
  return name == "lb" || name == "lh" || name == "lw" || name == "lbu" || name == "lhu" || name == "flw";
}

static bool is_scalar_store(std::string_view name) { return name == "sb" || name == "sh" || name == "sw" || name == "fsw"; }

static bool is_uncond_jump(const sbt::DecodedInst &di) {
  return di.name == "jal" && di.rd_class == sbt::RegClass::X && di.rd == 0 && di.imm_kind == sbt::ImmKind::J21;
}

static bool is_call(const sbt::DecodedInst &di) {
  return di.name == "jal" && di.rd_class == sbt::RegClass::X && di.rd != 0 && di.imm_kind == sbt::ImmKind::J21;
}

static bool is_ret(const sbt::DecodedInst &di) {
  return di.name == "jalr" && di.rd_class == sbt::RegClass::X && di.rs1_class == sbt::RegClass::X && di.rd == 0 && di.rs1 == 1 &&
         di.imm_kind == sbt::ImmKind::I12 && di.imm == 0;
}

static void require(bool ok, const EmitError &err) {
  if (!ok) throw err;
}

} // namespace

bool is_inlined_builtin_call_name(std::string_view callee) {
  return callee == "_Z13get_global_idj" || callee == "_Z12get_local_idj" || callee == "_Z12get_group_idj" || callee == "_Z15get_global_sizej" ||
         callee == "__builtin_riscv_workitem_id_x" || callee == "__builtin_riscv_workitem_id_y" || callee == "__builtin_riscv_workitem_id_z" ||
         callee == "__builtin_riscv_workgroup_id_x" || callee == "__builtin_riscv_workgroup_id_y" || callee == "__builtin_riscv_workgroup_id_z" ||
         callee == "__builtin_riscv_global_id_x" || callee == "__builtin_riscv_global_id_y" || callee == "__builtin_riscv_global_id_z" ||
         callee == "_Z10__clc_sqrtf" || callee == "_Z4sqrtf" ||
         // OpenCL float helpers.
         callee == "_Z4fmaxff" ||
         // PoCL trig example (float4).
         callee == "_Z3cosDv4_f" || callee == "_Z3sinDv4_f" || callee == "_Z3tanDv4_f" || callee == "_Z4sqrtDv4_f" || callee == "_Z4fabsDv4_f" ||
         // OpenCL integer helpers.
         callee == "_Z5mad24iii";
}

EmitError::EmitError(std::string code_, std::string func_, uint32_t pc_, std::string detail)
    : std::runtime_error(code_ + " func=" + func_ + " pc=" + hex_u32(pc_) + (detail.empty() ? "" : (" " + detail))),
      code(std::move(code_)),
      func(std::move(func_)),
      pc(pc_) {}

namespace {

struct ModuleInfo final {
  bool need_vctx = false;
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
  int tmp_label_id = 0;
  bool emitted_fp_dyn_note = false;

  EmitCtx(const sbt::cfg::FunctionCfg &cfg_, const std::unordered_map<uint32_t, std::string> &sym_by_addr_,
          const std::string &func_name_, const std::string &ptx_name_, const Options &opt_, const ModuleInfo &mod_, bool is_entry_)
      : cfg(cfg_), sym_by_addr(sym_by_addr_), func_name(func_name_), ptx_name(ptx_name_), opt(opt_), mod(mod_), is_entry(is_entry_) {}

  // Register assignment conventions (must match PTX declarations).
  // %r0: laneid
  // %r1: activemask
  // %r2: leader lane index
  // %p0: is_leader
  // %rd0: elf_base (global)
  // %rd1: heap_base (global)
  // %rd2: shmem_base (shared)
  // %rd3: wctx_ptr (shared)
  // %rd4: numeric-shared base (shared)  [shared_base_vaddr ..)  (stack + LDS)
  // %rd6: per-thread v-reg context base (local) for cross-`.func` calls
  // %r28: pds_base_vaddr (u32 Ventus numeric address)
  // %r29: pds_size_per_thread (u32 bytes)

  uint32_t x_off(int idx) const { return static_cast<uint32_t>(idx) * 4u; }

  std::string new_label(std::string_view kind) { return "_sbt_" + std::string(kind) + "_" + std::to_string(tmp_label_id++); }

  std::string bb_of_pc(uint32_t pc) const {
    auto it = cfg.inst_pc_to_block.find(pc);
    if (it == cfg.inst_pc_to_block.end()) return "";
    return label_bb(it->second);
  }

  void emit_line(const std::string &s) { out << "  " << s << "\n"; }
  void emit_raw(const std::string &s) { out << s; }
  void emit_label(const std::string &name) { out << name << ":\n"; }

  void emit_block_preamble() {
    // activemask + leader predicate (active-lane leader).
    emit_line("activemask.b32 " + r(1) + ";");
    emit_line("bfind.u32 " + r(2) + ", " + r(1) + ";");
    emit_line("setp.eq.u32 " + p(0) + ", " + r(0) + ", " + r(2) + ";");
  }

  void emit_warp_sync() { emit_line("bar.warp.sync " + r(1) + ";"); }

  void emit_vctx_store_all(uint32_t pc_for_err) {
    require(mod.need_vctx, EmitError("invalid.vctx", func_name, pc_for_err, "need_vctx=false"));
    for (int i = 0; i < 256; ++i) {
      const uint32_t off = static_cast<uint32_t>(i) * 4u;
      emit_line("st.local.u32 [" + rd(6) + "+" + std::to_string(off) + "], " + v(i) + ";");
    }
  }

  void emit_vctx_load_all(uint32_t pc_for_err) {
    require(mod.need_vctx, EmitError("invalid.vctx", func_name, pc_for_err, "need_vctx=false"));
    for (int i = 0; i < 256; ++i) {
      const uint32_t off = static_cast<uint32_t>(i) * 4u;
      emit_line("ld.local.u32 " + v(i) + ", [" + rd(6) + "+" + std::to_string(off) + "];");
    }
  }

  void emit_ld_x_u32_leader(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
    if (xreg == 0) {
      emit_line("@"+p(0)+" mov.u32 " + dst_r + ", 0;");
      return;
    }
    require(xreg >= 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<0"));
    emit_line("@" + p(0) + " ld.shared.u32 " + dst_r + ", [" + rd(3) + "+" + std::to_string(x_off(xreg)) + "];");
  }

  void emit_ld_x_u32_all(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
    if (xreg == 0) {
      emit_line("mov.u32 " + dst_r + ", 0;");
      return;
    }
    require(xreg >= 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<0"));
    emit_line("ld.shared.u32 " + dst_r + ", [" + rd(3) + "+" + std::to_string(x_off(xreg)) + "];");
  }

  void emit_st_x_u32_leader(int xreg, const std::string &src_r, uint32_t pc_for_err) {
    if (xreg == 0) return; // x0 is hard-wired zero.
    require(xreg > 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<=0"));
    emit_line("@" + p(0) + " st.shared.u32 [" + rd(3) + "+" + std::to_string(x_off(xreg)) + "], " + src_r + ";");
  }

  void emit_st_x_u32_all(int xreg, const std::string &src_r, uint32_t pc_for_err) {
    if (xreg == 0) return; // x0 is hard-wired zero.
    require(xreg > 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<=0"));
    emit_line("st.shared.u32 [" + rd(3) + "+" + std::to_string(x_off(xreg)) + "], " + src_r + ";");
  }

  bool scalar_leader_only() const { return opt.scalar_exec_leader_only; }

  std::string scalar_prefix() const { return scalar_leader_only() ? ("@" + p(0) + " ") : ""; }

  void maybe_note_fp_dyn_rm(uint32_t pc_for_err) {
    if (!opt.include_comments) return;
    if (emitted_fp_dyn_note) return;
    emit_line("// NOTE: scalar FP rm=DYN treated as RNE (CSR.frm not modeled)");
    emitted_fp_dyn_note = true;
    (void)pc_for_err;
  }

  sbt::FpRoundingMode normalize_fp_rm(sbt::FpRoundingMode rm, uint32_t pc_for_err) {
    if (rm == sbt::FpRoundingMode::None) return rm;
    if (rm == sbt::FpRoundingMode::DYN) {
      maybe_note_fp_dyn_rm(pc_for_err);
      return sbt::FpRoundingMode::RNE;
    }
    if (rm == sbt::FpRoundingMode::RMM || rm == sbt::FpRoundingMode::Reserved5 || rm == sbt::FpRoundingMode::Reserved6) {
      throw EmitError("unsupported.fp_rm", func_name, pc_for_err, "rm=" + std::string(sbt::to_string(rm)));
    }
    return rm;
  }

  std::string ptx_rm_f32(sbt::FpRoundingMode rm, uint32_t pc_for_err) {
    rm = normalize_fp_rm(rm, pc_for_err);
    switch (rm) {
    case sbt::FpRoundingMode::RNE: return ".rn";
    case sbt::FpRoundingMode::RTZ: return ".rz";
    case sbt::FpRoundingMode::RDN: return ".rm";
    case sbt::FpRoundingMode::RUP: return ".rp";
    case sbt::FpRoundingMode::None: default: throw EmitError("invalid.fp_rm", func_name, pc_for_err, "rm=none");
    }
  }

  std::string ptx_rm_cvt_i32(sbt::FpRoundingMode rm, uint32_t pc_for_err) {
    rm = normalize_fp_rm(rm, pc_for_err);
    switch (rm) {
    case sbt::FpRoundingMode::RNE: return ".rni";
    case sbt::FpRoundingMode::RTZ: return ".rzi";
    case sbt::FpRoundingMode::RDN: return ".rmi";
    case sbt::FpRoundingMode::RUP: return ".rpi";
    case sbt::FpRoundingMode::None: default: throw EmitError("invalid.fp_rm", func_name, pc_for_err, "rm=none");
    }
  }

  void emit_ld_x_u32_scalar(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
    if (scalar_leader_only()) emit_ld_x_u32_leader(dst_r, xreg, pc_for_err);
    else emit_ld_x_u32_all(dst_r, xreg, pc_for_err);
  }

  void emit_st_x_u32_scalar(int xreg, const std::string &src_r, uint32_t pc_for_err) {
    if (scalar_leader_only()) {
      emit_st_x_u32_leader(xreg, src_r, pc_for_err);
      if (xreg != 0) emit_warp_sync();
    } else {
      emit_st_x_u32_all(xreg, src_r, pc_for_err);
    }
  }

  void emit_addr_map_and_ld_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    if (scalar_leader_only()) emit_addr_map_and_ld_u32_leader(dst_r, addr_r, pc_for_err);
    else emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
  }

  void emit_addr_map_and_st_u32_scalar(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    if (scalar_leader_only()) {
      emit_addr_map_and_st_u32_leader(addr_r, src_r, pc_for_err);
      emit_warp_sync();
    } else {
      emit_addr_map_and_st_u32(addr_r, src_r, pc_for_err);
    }
  }

  void emit_addr_map_and_ld_u8_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    if (scalar_leader_only()) emit_addr_map_and_ld_u8_zext_u32_leader(dst_r, addr_r, pc_for_err);
    else emit_addr_map_and_ld_u8_zext_u32(dst_r, addr_r, pc_for_err);
  }

  void emit_addr_map_and_ld_u16_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    if (scalar_leader_only()) emit_addr_map_and_ld_u16_zext_u32_leader(dst_r, addr_r, pc_for_err);
    else emit_addr_map_and_ld_u16_zext_u32(dst_r, addr_r, pc_for_err);
  }

  void emit_addr_map_and_st_u8_scalar(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
    if (scalar_leader_only()) {
      emit_addr_map_and_st_u8_leader(addr_r, src_u8, pc_for_err);
      emit_warp_sync();
    } else {
      emit_addr_map_and_st_u8(addr_r, src_u8, pc_for_err);
    }
  }

  void emit_addr_map_and_st_u16_scalar(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
    if (scalar_leader_only()) {
      emit_addr_map_and_st_u16_leader(addr_r, src_u16, pc_for_err);
      emit_warp_sync();
    } else {
      emit_addr_map_and_st_u16(addr_r, src_u16, pc_for_err);
    }
  }

  void emit_addr_map_and_ld_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    // Copy to a scratch register to avoid aliasing with r16 temporaries (addr_r can be %r16 in prologue).
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");

    // shared predicate: addr in [shared_base_vaddr, elf_base_vaddr)
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared

    // heap predicate: addr >= heap_base_vaddr (else: ELF global)
    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.heap_base_vaddr) + ";"); // p4 = is_heap

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(4) + ", " + rd(16) + ";");

    // global_ptr -> rd16 (default: ELF); heap overwrites rd16 under @p4
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");

    emit_line("@" + p(4) + " add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("@" + p(4) + " cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("@" + p(4) + " add.u64 " + rd(16) + ", " + rd(1) + ", " + rd(16) + ";");

    emit_line("@" + p(3) + " ld.shared.u32 " + dst_r + ", [" + rd(17) + "];");
    emit_line("@!" + p(3) + " ld.global.u32 " + dst_r + ", [" + rd(16) + "];");
    (void)pc_for_err;
  }

  void emit_addr_map_and_ld_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_ld32_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_st_u32(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared

    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.heap_base_vaddr) + ";"); // p4 = is_heap

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(4) + ", " + rd(16) + ";");

    // global_ptr -> rd16 (default: ELF); heap overwrites rd16 under @p4
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");

    emit_line("@" + p(4) + " add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("@" + p(4) + " cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("@" + p(4) + " add.u64 " + rd(16) + ", " + rd(1) + ", " + rd(16) + ";");

    emit_line("@" + p(3) + " st.shared.u32 [" + rd(17) + "], " + src_r + ";");
    emit_line("@!" + p(3) + " st.global.u32 [" + rd(16) + "], " + src_r + ";");
    (void)pc_for_err;
  }

  void emit_addr_map_and_st_u32_leader(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_st32_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_st_u32(addr_r, src_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_ld_u8_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared

    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.heap_base_vaddr) + ";"); // p4 = is_heap

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(4) + ", " + rd(16) + ";");

    // global_ptr -> rd16 (default: ELF); heap overwrites rd16 under @p4
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");

    emit_line("@" + p(4) + " add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("@" + p(4) + " cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("@" + p(4) + " add.u64 " + rd(16) + ", " + rd(1) + ", " + rd(16) + ";");

    emit_line("@" + p(3) + " ld.shared.u8 " + u8(0) + ", [" + rd(17) + "];");
    emit_line("@!" + p(3) + " ld.global.u8 " + u8(0) + ", [" + rd(16) + "];");
    emit_line("cvt.u32.u8 " + dst_r + ", " + u8(0) + ";");
    (void)pc_for_err;
  }

  void emit_addr_map_and_ld_u16_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared

    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.heap_base_vaddr) + ";"); // p4 = is_heap

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(4) + ", " + rd(16) + ";");

    // global_ptr -> rd16 (default: ELF); heap overwrites rd16 under @p4
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");

    emit_line("@" + p(4) + " add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("@" + p(4) + " cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("@" + p(4) + " add.u64 " + rd(16) + ", " + rd(1) + ", " + rd(16) + ";");

    emit_line("@" + p(3) + " ld.shared.u16 " + u16(0) + ", [" + rd(17) + "];");
    emit_line("@!" + p(3) + " ld.global.u16 " + u16(0) + ", [" + rd(16) + "];");
    emit_line("cvt.u32.u16 " + dst_r + ", " + u16(0) + ";");
    (void)pc_for_err;
  }

  void emit_addr_map_and_ld_u8_zext_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_ldu8_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_ld_u8_zext_u32(dst_r, addr_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_ld_u16_zext_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_ldu16_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_ld_u16_zext_u32(dst_r, addr_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_st_u8(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared

    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.heap_base_vaddr) + ";"); // p4 = is_heap

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(4) + ", " + rd(16) + ";");

    // global_ptr -> rd16 (default: ELF); heap overwrites rd16 under @p4
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");

    emit_line("@" + p(4) + " add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("@" + p(4) + " cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("@" + p(4) + " add.u64 " + rd(16) + ", " + rd(1) + ", " + rd(16) + ";");

    emit_line("@" + p(3) + " st.shared.u8 [" + rd(17) + "], " + src_u8 + ";");
    emit_line("@!" + p(3) + " st.global.u8 [" + rd(16) + "], " + src_u8 + ";");
    (void)pc_for_err;
  }

  void emit_addr_map_and_st_u16(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared

    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.heap_base_vaddr) + ";"); // p4 = is_heap

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(4) + ", " + rd(16) + ";");

    // global_ptr -> rd16 (default: ELF); heap overwrites rd16 under @p4
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.elf_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");

    emit_line("@" + p(4) + " add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("@" + p(4) + " cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("@" + p(4) + " add.u64 " + rd(16) + ", " + rd(1) + ", " + rd(16) + ";");

    emit_line("@" + p(3) + " st.shared.u16 [" + rd(17) + "], " + src_u16 + ";");
    emit_line("@!" + p(3) + " st.global.u16 [" + rd(16) + "], " + src_u16 + ";");
    (void)pc_for_err;
  }

  void emit_addr_map_and_st_u8_leader(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_stu8_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_st_u8(addr_r, src_u8, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_st_u16_leader(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_stu16_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_st_u16(addr_r, src_u16, pc_for_err);
    emit_label(L_after);
  }

  void emit_builtin_get_id(std::string_view kind, uint32_t pc_for_err) {
    // Uses %v0 as "dim" input for get_*_idj; writes result to %v0.
    // kind: "global"|"local"|"group"
    emit_line("// builtin: " + std::string(kind) + "_id(dim=v0) -> v0");
    emit_line("mov.u32 " + r(20) + ", " + v(0) + ";"); // dim

    const std::string L_dim0 = new_label("dim0");
    const std::string L_dim1 = new_label("dim1");
    const std::string L_dim2 = new_label("dim2");
    const std::string L_done = new_label("dim_done");

    emit_line("setp.eq.u32 " + p(5) + ", " + r(20) + ", 0;");
    emit_line("@" + p(5) + " bra " + L_dim0 + ";");
    emit_line("setp.eq.u32 " + p(6) + ", " + r(20) + ", 1;");
    emit_line("@" + p(6) + " bra " + L_dim1 + ";");
    emit_line("setp.eq.u32 " + p(7) + ", " + r(20) + ", 2;");
    emit_line("@" + p(7) + " bra " + L_dim2 + ";");

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
        emit_line("mov.u32 " + r(21) + ", " + std::string(tid) + ";");
        emit_line("mov.u32 " + r(22) + ", " + std::string(ntid) + ";");
        emit_line("mov.u32 " + r(23) + ", " + std::string(ctaid) + ";");
        emit_line("mul.lo.u32 " + r(24) + ", " + r(23) + ", " + r(22) + ";");
        emit_line("add.u32 " + r(24) + ", " + r(24) + ", " + r(21) + ";");
        emit_line("mov.u32 " + v(0) + ", " + r(24) + ";");
      }
      emit_line("bra " + L_done + ";");
    };

    emit_dim(L_dim0, "%tid.x", "%ntid.x", "%ctaid.x");
    emit_dim(L_dim1, "%tid.y", "%ntid.y", "%ctaid.y");
    emit_dim(L_dim2, "%tid.z", "%ntid.z", "%ctaid.z");

    emit_label(L_done);
    (void)pc_for_err;
  }

  void emit_builtin_get_global_size(uint32_t pc_for_err) {
    // Uses %v0 as "dim" input for get_global_sizej; writes result to %v0.
    // Returns the launched global size, i.e. gridDim * blockDim per dimension.
    emit_line("// builtin: global_size(dim=v0) -> v0");
    emit_line("mov.u32 " + r(20) + ", " + v(0) + ";"); // dim

    const std::string L_dim0 = new_label("gsize_dim0");
    const std::string L_dim1 = new_label("gsize_dim1");
    const std::string L_dim2 = new_label("gsize_dim2");
    const std::string L_done = new_label("gsize_done");

    emit_line("setp.eq.u32 " + p(5) + ", " + r(20) + ", 0;");
    emit_line("@" + p(5) + " bra " + L_dim0 + ";");
    emit_line("setp.eq.u32 " + p(6) + ", " + r(20) + ", 1;");
    emit_line("@" + p(6) + " bra " + L_dim1 + ";");
    emit_line("setp.eq.u32 " + p(7) + ", " + r(20) + ", 2;");
    emit_line("@" + p(7) + " bra " + L_dim2 + ";");

    // For out-of-range dims, return 1 (matches typical OpenCL behavior for unused dims).
    emit_line("mov.u32 " + v(0) + ", 1;");
    emit_line("bra " + L_done + ";");

    auto emit_dim = [&](const std::string &L, const char *ntid, const char *nctaid) {
      emit_label(L);
      emit_line("mov.u32 " + r(21) + ", " + std::string(ntid) + ";");
      emit_line("mov.u32 " + r(22) + ", " + std::string(nctaid) + ";");
      emit_line("mul.lo.u32 " + r(23) + ", " + r(21) + ", " + r(22) + ";");
      emit_line("mov.u32 " + v(0) + ", " + r(23) + ";");
      emit_line("bra " + L_done + ";");
    };

    emit_dim(L_dim0, "%ntid.x", "%nctaid.x");
    emit_dim(L_dim1, "%ntid.y", "%nctaid.y");
    emit_dim(L_dim2, "%ntid.z", "%nctaid.z");

    emit_label(L_done);
    (void)pc_for_err;
  }

  void emit_builtin_fmaxff(uint32_t pc_for_err) {
    // OpenCL/C: float fmax(float a, float b)
    // Calling convention (ventus clc): a=v0, b=v1, ret=v0 (per-thread scalar).
    // Semantics: return the numeric maximum; if exactly one is NaN, return the other; if both NaN, return NaN.
    emit_line("// builtin: fmax(v0,v1) -> v0 (f32 bits, NaN-safe)");
    emit_line("mov.b32 " + f(0) + ", " + v(0) + ";");
    emit_line("mov.b32 " + f(1) + ", " + v(1) + ";");
    emit_line("setp.nan.f32 " + p(2) + ", " + f(0) + ", " + f(0) + ";");
    emit_line("setp.nan.f32 " + p(3) + ", " + f(1) + ", " + f(1) + ";");
    emit_line("max.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
    emit_line("selp.b32 " + f(2) + ", " + f(1) + ", " + f(2) + ", " + p(2) + ";"); // if a is NaN => b
    emit_line("selp.b32 " + f(2) + ", " + f(0) + ", " + f(2) + ", " + p(3) + ";"); // if b is NaN => a
    emit_line("mov.b32 " + v(0) + ", " + f(2) + ";");
    (void)pc_for_err;
  }

  void emit_builtin_sqrtf(uint32_t pc_for_err) {
    emit_line("// builtin: sqrtf(v0) -> v0 (f32 bits)");
    emit_line("mov.b32 " + f(0) + ", " + v(0) + ";");
    emit_line("sqrt.rn.f32 " + f(1) + ", " + f(0) + ";");
    emit_line("mov.b32 " + v(0) + ", " + f(1) + ";");
    (void)pc_for_err;
  }

  void emit_builtin_unary_f32_inplace(std::string_view opname, const std::string &dst_v, uint32_t pc_for_err) {
    emit_line("mov.b32 " + f(0) + ", " + dst_v + ";");
    if (opname == "sqrt") {
      emit_line("sqrt.rn.f32 " + f(1) + ", " + f(0) + ";");
    } else if (opname == "cos") {
      emit_line("cos.approx.f32 " + f(1) + ", " + f(0) + ";");
    } else if (opname == "sin") {
      emit_line("sin.approx.f32 " + f(1) + ", " + f(0) + ";");
    } else {
      throw EmitError("unsupported.call", func_name, pc_for_err, "unary_op=" + std::string(opname));
    }
    emit_line("mov.b32 " + dst_v + ", " + f(1) + ";");
  }

  void emit_builtin_vec4_cos(uint32_t pc_for_err) {
    emit_line("// builtin: cos(float4) in v0..v3");
    for (int i = 0; i < 4; ++i) emit_builtin_unary_f32_inplace("cos", v(i), pc_for_err);
  }

  void emit_builtin_vec4_sin(uint32_t pc_for_err) {
    emit_line("// builtin: sin(float4) in v0..v3");
    for (int i = 0; i < 4; ++i) emit_builtin_unary_f32_inplace("sin", v(i), pc_for_err);
  }

  void emit_builtin_vec4_sqrt(uint32_t pc_for_err) {
    emit_line("// builtin: sqrt(float4) in v0..v3");
    for (int i = 0; i < 4; ++i) emit_builtin_unary_f32_inplace("sqrt", v(i), pc_for_err);
  }

  void emit_builtin_vec4_fabs(uint32_t pc_for_err) {
    emit_line("// builtin: fabs(float4) in v0..v3 (bitwise clear sign)");
    for (int i = 0; i < 4; ++i) emit_line("and.b32 " + v(i) + ", " + v(i) + ", 0x7fffffff;");
    (void)pc_for_err;
  }

  void emit_builtin_vec4_tan(uint32_t pc_for_err) {
    emit_line("// builtin: tan(float4) in v0..v3 (approx via sin/cos)");
    for (int i = 0; i < 4; ++i) {
      emit_line("mov.b32 " + f(0) + ", " + v(i) + ";");
      emit_line("sin.approx.f32 " + f(1) + ", " + f(0) + ";");
      emit_line("cos.approx.f32 " + f(2) + ", " + f(0) + ";");
      emit_line("div.rn.f32 " + f(3) + ", " + f(1) + ", " + f(2) + ";");
      emit_line("mov.b32 " + v(i) + ", " + f(3) + ";");
    }
    (void)pc_for_err;
  }

  void emit_builtin_mad24iii(uint32_t pc_for_err) {
    // OpenCL: int mad24(int a, int b, int c) => mul24(a,b) + c (signed 24-bit multiply).
    // Calling convention (ventus clc): a=v0, b=v1, c=v2, ret=v0.
    emit_line("// builtin: mad24(v0,v1,v2) -> v0 (signed 24-bit)");
    emit_line("mov.b32 " + r(14) + ", " + v(0) + ";");
    emit_line("mov.b32 " + r(15) + ", " + v(1) + ";");
    emit_line("mov.b32 " + r(16) + ", " + v(2) + ";");
    // sign-extend low 24 bits: (x << 8) >> 8
    emit_line("shl.b32 " + r(14) + ", " + r(14) + ", 8;");
    emit_line("shr.s32 " + r(14) + ", " + r(14) + ", 8;");
    emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 8;");
    emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 8;");
    emit_line("mad.lo.s32 " + r(14) + ", " + r(14) + ", " + r(15) + ", " + r(16) + ";");
    emit_line("mov.b32 " + v(0) + ", " + r(14) + ";");
    (void)pc_for_err;
  }

  void emit_direct_call(const std::string &callee_ptx) {
    const std::string p_elf = new_label("call_elf");
    const std::string p_heap = new_label("call_heap");
    const std::string p_wctx = new_label("call_wctx");
    const std::string p_lds = new_label("call_lds");
    const std::string p_knl = new_label("call_knl");
    const std::string p_pds_base = new_label("call_pds_base_vaddr");
    const std::string p_pds_size = new_label("call_pds_size_per_thread");
    const std::string p_wid = new_label("call_wid");
    const std::string p_numw = new_label("call_numw");
    const std::string p_vctx = new_label("call_vctx");

    emit_line(".param .u64 " + p_elf + ";");
    emit_line(".param .u64 " + p_heap + ";");
    emit_line(".param .u64 " + p_wctx + ";");
    emit_line(".param .u64 " + p_lds + ";");
    emit_line(".param .u32 " + p_knl + ";");
    emit_line(".param .u32 " + p_pds_base + ";");
    emit_line(".param .u32 " + p_pds_size + ";");
    emit_line(".param .u32 " + p_wid + ";");
    emit_line(".param .u32 " + p_numw + ";");
    if (mod.need_vctx) emit_line(".param .u64 " + p_vctx + ";");

    emit_line("st.param.u64 [" + p_elf + "], " + rd(0) + ";");
    emit_line("st.param.u64 [" + p_heap + "], " + rd(1) + ";");
    emit_line("st.param.u64 [" + p_wctx + "], " + rd(3) + ";");
    emit_line("st.param.u64 [" + p_lds + "], " + rd(4) + ";");
    emit_line("st.param.u32 [" + p_knl + "], " + r(30) + ";");
    emit_line("st.param.u32 [" + p_pds_base + "], " + r(28) + ";");
    emit_line("st.param.u32 [" + p_pds_size + "], " + r(29) + ";");
    emit_line("st.param.u32 [" + p_wid + "], " + r(10) + ";");
    emit_line("st.param.u32 [" + p_numw + "], " + r(12) + ";");
    if (mod.need_vctx) emit_line("st.param.u64 [" + p_vctx + "], " + rd(6) + ";");

    std::string args =
        p_elf + ", " + p_heap + ", " + p_wctx + ", " + p_lds + ", " + p_knl + ", " + p_pds_base + ", " + p_pds_size + ", " + p_wid + ", " + p_numw;
    if (mod.need_vctx) args += ", " + p_vctx;
    emit_line("call.uni " + callee_ptx + ", (" + args + ");");
  }

  void emit_scalar_fclass_s(const sbt::DecodedInst &di, uint32_t pc_for_err) {
    // RISC-V FCLASS.S: return a 10-bit class mask in rd (integer bits).
    // Bits: 0..9 = -inf, -norm, -subnorm, -0, +0, +subnorm, +norm, +inf, sNaN, qNaN
    emit_ld_x_u32_scalar(r(14), di.rs1, pc_for_err); // f32 bits
    emit_line(scalar_prefix() + "and.b32 " + r(15) + ", " + r(14) + ", 0x80000000;"); // sign bit
    emit_line(scalar_prefix() + "and.b32 " + r(16) + ", " + r(14) + ", 0x7f800000;"); // exp bits
    emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(14) + ", 0x007fffff;"); // frac bits

    emit_line(scalar_prefix() + "setp.ne.u32 " + p(1) + ", " + r(15) + ", 0;");          // sign
    emit_line(scalar_prefix() + "setp.eq.u32 " + p(2) + ", " + r(16) + ", 0;");          // exp==0
    emit_line(scalar_prefix() + "setp.eq.u32 " + p(3) + ", " + r(16) + ", 0x7f800000;"); // exp==all1
    emit_line(scalar_prefix() + "setp.eq.u32 " + p(4) + ", " + r(17) + ", 0;");          // frac==0

    // is_zero = exp==0 && frac==0
    emit_line(scalar_prefix() + "and.pred " + p(5) + ", " + p(2) + ", " + p(4) + ";");
    // is_sub = exp==0 && frac!=0
    emit_line(scalar_prefix() + "not.pred " + p(6) + ", " + p(4) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(6) + ", " + p(2) + ", " + p(6) + ";");
    // is_inf = exp==all1 && frac==0
    emit_line(scalar_prefix() + "and.pred " + p(7) + ", " + p(3) + ", " + p(4) + ";");
    // is_nan = exp==all1 && frac!=0
    emit_line(scalar_prefix() + "not.pred " + p(8) + ", " + p(4) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(8) + ", " + p(3) + ", " + p(8) + ";");
    // is_norm = !exp==0 && !exp==all1
    emit_line(scalar_prefix() + "not.pred " + p(9) + ", " + p(2) + ";");
    emit_line(scalar_prefix() + "not.pred " + p(10) + ", " + p(3) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(9) + ", " + p(9) + ", " + p(10) + ";");

    // qnan = is_nan && (frac[22]==1)
    emit_line(scalar_prefix() + "and.b32 " + r(18) + ", " + r(17) + ", 0x00400000;");
    emit_line(scalar_prefix() + "setp.ne.u32 " + p(10) + ", " + r(18) + ", 0;");
    emit_line(scalar_prefix() + "and.pred " + p(10) + ", " + p(8) + ", " + p(10) + ";"); // qnan
    // snan = is_nan && !qnan
    emit_line(scalar_prefix() + "not.pred " + p(11) + ", " + p(10) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(11) + ", " + p(8) + ", " + p(11) + ";"); // snan

    emit_line(scalar_prefix() + "mov.u32 " + r(19) + ", 0;");

    // Derive per-class predicates.
    // -inf / +inf
    emit_line(scalar_prefix() + "and.pred " + p(12) + ", " + p(7) + ", " + p(1) + ";"); // -inf
    emit_line(scalar_prefix() + "not.pred " + p(13) + ", " + p(1) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(13) + ", " + p(7) + ", " + p(13) + ";"); // +inf
    // -0 / +0
    emit_line(scalar_prefix() + "and.pred " + p(14) + ", " + p(5) + ", " + p(1) + ";"); // -0
    emit_line(scalar_prefix() + "not.pred " + p(15) + ", " + p(1) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(15) + ", " + p(5) + ", " + p(15) + ";"); // +0

    auto or_if = [&](const std::string &pred, uint32_t bit) {
      emit_line(scalar_prefix() + "selp.u32 " + r(20) + ", " + std::to_string(bit) + ", 0, " + pred + ";");
      emit_line(scalar_prefix() + "or.b32 " + r(19) + ", " + r(19) + ", " + r(20) + ";");
    };

    or_if(p(12), 1u);    // -inf
    or_if(p(13), 128u);  // +inf
    or_if(p(14), 8u);    // -0
    or_if(p(15), 16u);   // +0

    // -sub / +sub
    emit_line(scalar_prefix() + "and.pred " + p(12) + ", " + p(6) + ", " + p(1) + ";"); // -sub
    emit_line(scalar_prefix() + "not.pred " + p(13) + ", " + p(1) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(13) + ", " + p(6) + ", " + p(13) + ";"); // +sub
    or_if(p(12), 4u);
    or_if(p(13), 32u);

    // -norm / +norm
    emit_line(scalar_prefix() + "and.pred " + p(12) + ", " + p(9) + ", " + p(1) + ";"); // -norm
    emit_line(scalar_prefix() + "not.pred " + p(13) + ", " + p(1) + ";");
    emit_line(scalar_prefix() + "and.pred " + p(13) + ", " + p(9) + ", " + p(13) + ";"); // +norm
    or_if(p(12), 2u);
    or_if(p(13), 64u);

    // NaNs
    or_if(p(11), 256u); // sNaN
    or_if(p(10), 512u); // qNaN

    emit_st_x_u32_scalar(di.rd, r(19), pc_for_err);
  }

  bool try_emit_scalar_fp(const sbt::DecodedInst &di) {
    const uint32_t pc = di.pc;

    // Bitwise moves (Zfinx: both sides are X regs).
    if (di.name == "fmv_w_x" || di.name == "fmv_x_w") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return true;
    }

    // Sign injection.
    if (di.name == "fsgnj_s" || di.name == "fsgnjn_s" || di.name == "fsgnjx_s") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // magnitude source
      emit_ld_x_u32_scalar(r(15), di.rs2, pc); // sign source
      emit_line(scalar_prefix() + "and.b32 " + r(16) + ", " + r(14) + ", 0x7fffffff;");
      if (di.name == "fsgnj_s") {
        emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 0x80000000;");
      } else if (di.name == "fsgnjn_s") {
        emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 0x80000000;");
        emit_line(scalar_prefix() + "xor.b32 " + r(17) + ", " + r(17) + ", 0x80000000;");
      } else { // fsgnjx_s
        emit_line(scalar_prefix() + "xor.b32 " + r(17) + ", " + r(14) + ", " + r(15) + ";");
        emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(17) + ", 0x80000000;");
      }
      emit_line(scalar_prefix() + "or.b32 " + r(16) + ", " + r(16) + ", " + r(17) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return true;
    }

    // Arithmetic.
    auto f32_binop = [&](const char *op) {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + f(1) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + std::string(op) + rm + ".f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + r(16) + ", " + f(2) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
    };
    if (di.name == "fadd_s") { f32_binop("add"); return true; }
    if (di.name == "fsub_s") { f32_binop("sub"); return true; }
    if (di.name == "fmul_s") { f32_binop("mul"); return true; }
    if (di.name == "fdiv_s") { f32_binop("div"); return true; }
    if (di.name == "fsqrt_s") {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "sqrt" + rm + ".f32 " + f(1) + ", " + f(0) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + r(15) + ", " + f(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return true;
    }

    // FMA family.
    if (di.name == "fmadd_s" || di.name == "fmsub_s" || di.name == "fnmsub_s" || di.name == "fnmadd_s") {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // a
      emit_ld_x_u32_scalar(r(15), di.rs2, pc); // b
      emit_ld_x_u32_scalar(r(16), di.rs3, pc); // c
      if (di.name == "fmsub_s" || di.name == "fnmadd_s") emit_line(scalar_prefix() + "xor.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
      if (di.name == "fnmsub_s" || di.name == "fnmadd_s") emit_line(scalar_prefix() + "xor.b32 " + r(14) + ", " + r(14) + ", 0x80000000;");
      emit_line(scalar_prefix() + "mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + f(1) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + f(2) + ", " + r(16) + ";");
      emit_line(scalar_prefix() + "fma" + rm + ".f32 " + f(3) + ", " + f(0) + ", " + f(1) + ", " + f(2) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + r(17) + ", " + f(3) + ";");
      emit_st_x_u32_scalar(di.rd, r(17), pc);
      return true;
    }

    // Min/max (NaN-safe).
    if (di.name == "fmin_s" || di.name == "fmax_s") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + f(1) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "setp.nan.f32 " + p(2) + ", " + f(0) + ", " + f(0) + ";");
      emit_line(scalar_prefix() + "setp.nan.f32 " + p(3) + ", " + f(1) + ", " + f(1) + ";");
      emit_line(scalar_prefix() + std::string(di.name == "fmax_s" ? "max" : "min") + ".f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line(scalar_prefix() + "selp.b32 " + f(2) + ", " + f(1) + ", " + f(2) + ", " + p(2) + ";"); // if a is NaN => b
      emit_line(scalar_prefix() + "selp.b32 " + f(2) + ", " + f(0) + ", " + f(2) + ", " + p(3) + ";"); // if b is NaN => a
      emit_line(scalar_prefix() + "mov.b32 " + r(16) + ", " + f(2) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return true;
    }

    // Comparisons: exact 0/1 integer result.
    if (di.name == "feq_s" || di.name == "flt_s" || di.name == "fle_s") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + f(1) + ", " + r(15) + ";");
      if (di.name == "feq_s") emit_line(scalar_prefix() + "setp.eq.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      else if (di.name == "flt_s") emit_line(scalar_prefix() + "setp.lt.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      else emit_line(scalar_prefix() + "setp.le.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return true;
    }

    // Conversions.
    if (di.name == "fcvt_s_w" || di.name == "fcvt_s_wu") {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "cvt" + rm + ".f32." + (di.name == "fcvt_s_w" ? "s32 " : "u32 ") + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "mov.b32 " + r(15) + ", " + f(0) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return true;
    }
    if (di.name == "fcvt_w_s" || di.name == "fcvt_wu_s") {
      const std::string rm = ptx_rm_cvt_i32(di.fp_rm, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // f32 bits
      emit_line(scalar_prefix() + "mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "cvt" + rm + "." + (di.name == "fcvt_w_s" ? "s32" : "u32") + ".f32 " + r(15) + ", " + f(0) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return true;
    }

    if (di.name == "fclass_s") {
      emit_scalar_fclass_s(di, pc);
      return true;
    }

    return false;
  }

  void emit_one_inst(const sbt::cfg::BundleInst &bi) {
    const sbt::DecodedInst &di = bi.inst;
    const uint32_t pc = di.pc;

    if (opt.include_comments) {
      emit_line("// " + hex_u32(bi.pc) + " " + di.name);
    }

    // No-ops under structured translation.
    if (di.name == "setrpc" || di.name == "join" || di.name == "vsetvli") {
      return;
    }

    if (di.name == "endprg" || is_ret(di)) {
      if (!is_entry && mod.need_vctx) {
        emit_line("// spill v-regfile back to vctx for return");
        emit_vctx_store_all(pc);
      }
      emit_line("ret;");
      return;
    }

    if (di.name == "barrier") {
      emit_line("bar.sync 0;");
      return;
    }

    // Calls: support a small inlined builtin set, plus direct calls to emitted `.func`s.
    if (is_call(di)) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(pc) + static_cast<int64_t>(di.imm));
      auto it = sym_by_addr.find(target);
      require(it != sym_by_addr.end(), EmitError("unsupported.call", func_name, pc, "target=" + hex_u32(target)));
      const std::string &callee = it->second;

      // Write link register (uniform) for debugging parity; not used by inlined builtins.
      emit_line("@" + p(0) + " mov.u32 " + r(14) + ", " + hex_u32(pc + 4) + ";");
      emit_st_x_u32_leader(di.rd, r(14), pc);
      emit_warp_sync();

      if (is_inlined_builtin_call_name(callee)) {
        if (callee == "_Z13get_global_idj") {
          emit_builtin_get_id("global", pc);
        } else if (callee == "_Z12get_local_idj") {
          emit_builtin_get_id("local", pc);
        } else if (callee == "_Z12get_group_idj") {
          emit_builtin_get_id("group", pc);
        } else if (callee == "_Z15get_global_sizej") {
          emit_builtin_get_global_size(pc);
        } else if (callee == "_Z4fmaxff") {
          emit_builtin_fmaxff(pc);
        } else if (callee == "_Z10__clc_sqrtf" || callee == "_Z4sqrtf") {
          emit_builtin_sqrtf(pc);
        } else if (callee == "_Z3cosDv4_f") {
          emit_builtin_vec4_cos(pc);
        } else if (callee == "_Z3sinDv4_f") {
          emit_builtin_vec4_sin(pc);
        } else if (callee == "_Z3tanDv4_f") {
          emit_builtin_vec4_tan(pc);
        } else if (callee == "_Z4sqrtDv4_f") {
          emit_builtin_vec4_sqrt(pc);
        } else if (callee == "_Z4fabsDv4_f") {
          emit_builtin_vec4_fabs(pc);
        } else if (callee == "_Z5mad24iii") {
          emit_builtin_mad24iii(pc);
        } else if (callee == "__builtin_riscv_workitem_id_x") {
          emit_line("mov.u32 " + v(0) + ", %tid.x;");
        } else if (callee == "__builtin_riscv_workitem_id_y") {
          emit_line("mov.u32 " + v(0) + ", %tid.y;");
        } else if (callee == "__builtin_riscv_workitem_id_z") {
          emit_line("mov.u32 " + v(0) + ", %tid.z;");
        } else if (callee == "__builtin_riscv_workgroup_id_x") {
          emit_line("mov.u32 " + v(0) + ", %ctaid.x;");
        } else if (callee == "__builtin_riscv_workgroup_id_y") {
          emit_line("mov.u32 " + v(0) + ", %ctaid.y;");
        } else if (callee == "__builtin_riscv_workgroup_id_z") {
          emit_line("mov.u32 " + v(0) + ", %ctaid.z;");
        } else if (callee == "__builtin_riscv_global_id_x") {
          emit_line("mov.u32 " + r(21) + ", %tid.x;");
          emit_line("mov.u32 " + r(22) + ", %ntid.x;");
          emit_line("mov.u32 " + r(23) + ", %ctaid.x;");
          emit_line("mul.lo.u32 " + r(24) + ", " + r(23) + ", " + r(22) + ";");
          emit_line("add.u32 " + r(24) + ", " + r(24) + ", " + r(21) + ";");
          emit_line("mov.u32 " + v(0) + ", " + r(24) + ";");
        } else if (callee == "__builtin_riscv_global_id_y") {
          emit_line("mov.u32 " + r(21) + ", %tid.y;");
          emit_line("mov.u32 " + r(22) + ", %ntid.y;");
          emit_line("mov.u32 " + r(23) + ", %ctaid.y;");
          emit_line("mul.lo.u32 " + r(24) + ", " + r(23) + ", " + r(22) + ";");
          emit_line("add.u32 " + r(24) + ", " + r(24) + ", " + r(21) + ";");
          emit_line("mov.u32 " + v(0) + ", " + r(24) + ";");
        } else if (callee == "__builtin_riscv_global_id_z") {
          emit_line("mov.u32 " + r(21) + ", %tid.z;");
          emit_line("mov.u32 " + r(22) + ", %ntid.z;");
          emit_line("mov.u32 " + r(23) + ", %ctaid.z;");
          emit_line("mul.lo.u32 " + r(24) + ", " + r(23) + ", " + r(22) + ";");
          emit_line("add.u32 " + r(24) + ", " + r(24) + ", " + r(21) + ";");
          emit_line("mov.u32 " + v(0) + ", " + r(24) + ";");
        } else {
          throw EmitError("unsupported.call", func_name, pc, "callee=" + callee);
        }
        return;
      }

      require(mod.ptx_name_by_addr != nullptr, EmitError("unsupported.call", func_name, pc, "callee=" + callee));
      auto jt = mod.ptx_name_by_addr->find(target);
      require(jt != mod.ptx_name_by_addr->end(), EmitError("unsupported.call", func_name, pc, "callee=" + callee));
      if (mod.need_vctx) {
        emit_line("// spill v-regfile to vctx for call");
        emit_vctx_store_all(pc);
      }
      emit_direct_call(jt->second);
      if (mod.need_vctx) {
        emit_line("// restore v-regfile from vctx after call");
        emit_vctx_load_all(pc);
      }
      return;
    }

    // Scalar jumps (unconditional).
    if (is_uncond_jump(di)) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(pc) + static_cast<int64_t>(di.imm));
      const std::string bb = bb_of_pc(target);
      require(!bb.empty(), EmitError("invalid.cfg", func_name, pc, "jump target " + hex_u32(target)));
      emit_line("bra " + bb + ";");
      return;
    }

    // Scalar conditional branches.
    if (is_scalar_branch(di.name) && di.imm_kind == sbt::ImmKind::B13) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(pc) + static_cast<int64_t>(di.imm));
      const uint32_t fallthrough = pc + 4;
      const std::string bb_t = bb_of_pc(target);
      const std::string bb_f = bb_of_pc(fallthrough);
      require(!bb_t.empty() && !bb_f.empty(), EmitError("invalid.cfg", func_name, pc, "branch target/fallthrough missing"));

      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_ld_x_u32_all(r(15), di.rs2, pc);

      if (di.name == "beq") emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bne") emit_line("setp.ne.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "blt") emit_line("setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bge") emit_line("setp.ge.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bltu") emit_line("setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bgeu") emit_line("setp.ge.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else throw EmitError("unsupported.inst", func_name, pc, di.name);

      emit_line("@" + p(1) + " bra " + bb_t + ";");
      emit_line("bra " + bb_f + ";");
      return;
    }

    // Vector conditional branches.
    if (is_vector_branch(di.name) && di.imm_kind == sbt::ImmKind::B13) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(pc) + static_cast<int64_t>(di.imm));
      const uint32_t fallthrough = pc + 4;
      const std::string bb_t = bb_of_pc(target);
      const std::string bb_f = bb_of_pc(fallthrough);
      require(!bb_t.empty() && !bb_f.empty(), EmitError("invalid.cfg", func_name, pc, "vbranch target/fallthrough missing"));

      // NOTE: Ventus vbranch encodes operands in a swapped order vs scalar branches:
      // llvm-objdump / cyclesim logs print `vb* v<rs2>, v<rs1>, off`.
      // Treat `rs2` as the left operand and `rs1` as the right operand.
      const std::string a = v(di.rs2);
      const std::string b = v(di.rs1);

      if (di.name == "vbeq") emit_line("setp.eq.u32 " + p(1) + ", " + a + ", " + b + ";");
      else if (di.name == "vbne") emit_line("setp.ne.u32 " + p(1) + ", " + a + ", " + b + ";");
      else if (di.name == "vblt") emit_line("setp.lt.s32 " + p(1) + ", " + a + ", " + b + ";");
      else if (di.name == "vbge") emit_line("setp.ge.s32 " + p(1) + ", " + a + ", " + b + ";");
      else if (di.name == "vbltu") emit_line("setp.lt.u32 " + p(1) + ", " + a + ", " + b + ";");
      else if (di.name == "vbgeu") emit_line("setp.ge.u32 " + p(1) + ", " + a + ", " + b + ";");
      else throw EmitError("unsupported.inst", func_name, pc, di.name);

      emit_line("@" + p(1) + " bra " + bb_t + ";");
      emit_line("bra " + bb_f + ";");
      return;
    }

    // CSR ops (prototype: treat Ventus CSRs as read-only for bring-up).
    if (di.name == "csrrw" || di.name == "csrrs" || di.name == "csrrc" || di.name == "csrrwi" || di.name == "csrrsi" || di.name == "csrrci") {
      require(di.imm_kind == sbt::ImmKind::CSR12, EmitError("invalid.csr", func_name, pc, "imm_kind"));
      const uint32_t csr = static_cast<uint32_t>(di.imm);

      // Compute CSR value to %r14 (uniform), then store the old value to x[rd].
      // NOTE: For now, csr writes are ignored (read-only model).
      if (csr == 0x803u) { // CSR_KNL
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + r(30) + ";"); // %r30 holds knl_vaddr (loaded in prologue)
      } else if (csr == 0x802u) { // CSR_NUMT
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", 32;");
      } else if (csr == 0x805u) { // CSR_WID
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + r(10) + ";"); // warp_id_in_block
      } else if (csr == 0x800u) { // CSR_TID
        emit_line(scalar_prefix() + "shl.b32 " + r(14) + ", " + r(10) + ", 5;");
      } else if (csr == 0x801u) { // CSR_NUMW
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + r(12) + ";"); // warps_per_block
      } else if (csr == 0x806u) { // CSR_LDS
        // CSR_LDS: base numeric address of work-group local memory (also xgpr spill base).
        // In this backend, numeric shared addresses start at `shared_base_vaddr`.
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
      } else if (csr == 0x807u) { // CSR_PDS
        // CSR_PDS: per-warp private memory base (numeric address).
        //
        // blk_linear = ctaid.x + nctaid.x * (ctaid.y + nctaid.y * ctaid.z)
        // warp_linear = blk_linear*warps_per_block + warp_id_in_block
        // CSR_PDS = pds_base_vaddr + warp_linear * (32 * pds_size_per_thread)
        emit_line(scalar_prefix() + "mov.u32 " + r(15) + ", %ctaid.x;");
        emit_line(scalar_prefix() + "mov.u32 " + r(16) + ", %ctaid.y;");
        emit_line(scalar_prefix() + "mov.u32 " + r(17) + ", %ctaid.z;");
        emit_line(scalar_prefix() + "mov.u32 " + r(18) + ", %nctaid.x;");
        emit_line(scalar_prefix() + "mov.u32 " + r(19) + ", %nctaid.y;");
        emit_line(scalar_prefix() + "mul.lo.u32 " + r(20) + ", " + r(19) + ", " + r(17) + ";");
        emit_line(scalar_prefix() + "add.u32 " + r(20) + ", " + r(20) + ", " + r(16) + ";");
        emit_line(scalar_prefix() + "mul.lo.u32 " + r(20) + ", " + r(18) + ", " + r(20) + ";");
        emit_line(scalar_prefix() + "add.u32 " + r(20) + ", " + r(20) + ", " + r(15) + ";"); // blk_linear

        emit_line(scalar_prefix() + "mul.lo.u32 " + r(21) + ", " + r(20) + ", " + r(12) + ";");
        emit_line(scalar_prefix() + "add.u32 " + r(21) + ", " + r(21) + ", " + r(10) + ";"); // warp_linear

        emit_line(scalar_prefix() + "shl.b32 " + r(22) + ", " + r(29) + ", 5;"); // bytes_per_warp
        emit_line(scalar_prefix() + "cvt.u64.u32 " + rd(16) + ", " + r(21) + ";");
        emit_line(scalar_prefix() + "cvt.u64.u32 " + rd(17) + ", " + r(22) + ";");
        emit_line(scalar_prefix() + "mul.lo.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
        emit_line(scalar_prefix() + "cvt.u64.u32 " + rd(17) + ", " + r(28) + ";");
        emit_line(scalar_prefix() + "add.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
        emit_line(scalar_prefix() + "cvt.u32.u64 " + r(14) + ", " + rd(16) + ";");
      } else if (csr == 0x808u) { // CSR_GDX
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.x;");
      } else if (csr == 0x809u) { // CSR_GDY
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.y;");
      } else if (csr == 0x80au) { // CSR_GDZ
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.z;");
      } else {
        throw EmitError("unsupported.csr", func_name, pc, "csr=" + hex_u32(csr));
      }

      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }

    // Scalar loads/stores (uniform ops).
    if (is_scalar_load(di.name) && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "add.u32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");

      // Scalar loads.
      if (di.name == "lw") {
        emit_addr_map_and_ld_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.name == "flw") {
        // Zfinx model: flw loads f32 bits into an X reg.
        emit_addr_map_and_ld_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.name == "lb") {
        emit_addr_map_and_ld_u8_zext_u32_scalar(r(17), r(15), pc);
        emit_line(scalar_prefix() + "shl.b32 " + r(17) + ", " + r(17) + ", 24;");
        emit_line(scalar_prefix() + "shr.s32 " + r(17) + ", " + r(17) + ", 24;");
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.name == "lh") {
        emit_addr_map_and_ld_u16_zext_u32_scalar(r(17), r(15), pc);
        emit_line(scalar_prefix() + "shl.b32 " + r(17) + ", " + r(17) + ", 16;");
        emit_line(scalar_prefix() + "shr.s32 " + r(17) + ", " + r(17) + ", 16;");
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.name == "lbu") {
        emit_addr_map_and_ld_u8_zext_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.name == "lhu") {
        emit_addr_map_and_ld_u16_zext_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      throw EmitError("unsupported.inst", func_name, pc, di.name);
    }

    if (is_scalar_store(di.name) && di.imm_kind == sbt::ImmKind::S12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // base
      emit_ld_x_u32_scalar(r(15), di.rs2, pc); // value
      emit_line(scalar_prefix() + "add.u32 " + r(16) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");

      // Scalar stores.
      if (di.name == "sw") {
        emit_addr_map_and_st_u32_scalar(r(16), r(15), pc);
        return;
      }
      if (di.name == "fsw") {
        // Zfinx model: fsw stores f32 bits from an X reg.
        emit_addr_map_and_st_u32_scalar(r(16), r(15), pc);
        return;
      }
      if (di.name == "sb") {
        emit_line(scalar_prefix() + "cvt.u8.u32 " + u8(1) + ", " + r(15) + ";");
        emit_addr_map_and_st_u8_scalar(r(16), u8(1), pc);
        return;
      }
      if (di.name == "sh") {
        emit_line(scalar_prefix() + "cvt.u16.u32 " + u16(1) + ", " + r(15) + ";");
        emit_addr_map_and_st_u16_scalar(r(16), u16(1), pc);
        return;
      }
      throw EmitError("unsupported.inst", func_name, pc, di.name);
    }

    // Scalar FP ops (Zfinx: f32 bits carried in X regs).
    if (try_emit_scalar_fp(di)) return;

    // Scalar ALU ops (uniform) - minimal subset used by Rodinia.
    if (di.name == "addi" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "add.s32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.name == "add") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "add.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "sub") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "sub.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "and") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "or") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "or.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "xor") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "xor.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "andi" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(15) + ", " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.name == "ori" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "or.b32 " + r(15) + ", " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.name == "mul") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mul.lo.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "mulh") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mul.hi.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "mulhu") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mul.hi.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "mulhsu") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "cvt.s64.s32 " + rd(18) + ", " + r(14) + ";");
      emit_line(scalar_prefix() + "cvt.u64.u32 " + rd(19) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "mul.lo.s64 " + rd(18) + ", " + rd(18) + ", " + rd(19) + ";");
      emit_line(scalar_prefix() + "shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
      emit_line(scalar_prefix() + "cvt.u32.u64 " + r(16) + ", " + rd(18) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "div" || di.name == "divu" || di.name == "rem" || di.name == "remu") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // dividend
      emit_ld_x_u32_scalar(r(15), di.rs2, pc); // divisor

      const bool signed_div = (di.name == "div" || di.name == "rem");
      if (scalar_leader_only()) {
        const std::string L_after = new_label("leader_div_after");
        emit_line("@!" + p(0) + " bra " + L_after + ";");

        emit_line("setp.eq.u32 " + p(1) + ", " + r(15) + ", 0;"); // p1 = div0
        if (signed_div) {
          emit_line("setp.eq.u32 " + p(2) + ", " + r(14) + ", 0x80000000;");
          emit_line("setp.eq.u32 " + p(3) + ", " + r(15) + ", 0xffffffff;");
          emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";"); // p2 = overflow
        } else {
          emit_line("setp.ne.u32 " + p(2) + ", 0, 0;"); // p2 = false
        }

        emit_line("not.pred " + p(4) + ", " + p(1) + ";");                 // p4 = !div0
        emit_line("not.pred " + p(5) + ", " + p(2) + ";");                 // p5 = !overflow
        emit_line("and.pred " + p(4) + ", " + p(4) + ", " + p(5) + ";");   // p4 = !div0 && !overflow

        if (di.name == "div") {
          emit_line("mov.u32 " + r(16) + ", 0xffffffff;"); // div by zero => -1
          emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0x80000000;"); // overflow => INT_MIN
          emit_line("@" + p(4) + " div.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        } else if (di.name == "divu") {
          emit_line("mov.u32 " + r(16) + ", 0xffffffff;"); // div by zero => all-ones
          emit_line("@" + p(4) + " div.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        } else if (di.name == "rem") {
          emit_line("mov.u32 " + r(16) + ", " + r(14) + ";"); // rem by zero => dividend
          emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0;"); // overflow => 0
          emit_line("@" + p(4) + " rem.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        } else { // remu
          emit_line("mov.u32 " + r(16) + ", " + r(14) + ";"); // rem by zero => dividend
          emit_line("@" + p(4) + " rem.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        }

        emit_label(L_after);
      } else {
        emit_line("setp.eq.u32 " + p(1) + ", " + r(15) + ", 0;"); // p1 = div0
        if (signed_div) {
          emit_line("setp.eq.u32 " + p(2) + ", " + r(14) + ", 0x80000000;");
          emit_line("setp.eq.u32 " + p(3) + ", " + r(15) + ", 0xffffffff;");
          emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";"); // p2 = overflow
        } else {
          emit_line("setp.ne.u32 " + p(2) + ", 0, 0;"); // p2 = false
        }

        emit_line("not.pred " + p(4) + ", " + p(1) + ";");                 // p4 = !div0
        emit_line("not.pred " + p(5) + ", " + p(2) + ";");                 // p5 = !overflow
        emit_line("and.pred " + p(4) + ", " + p(4) + ", " + p(5) + ";");   // p4 = !div0 && !overflow

        if (di.name == "div") {
          emit_line("mov.u32 " + r(16) + ", 0xffffffff;"); // div by zero => -1
          emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0x80000000;"); // overflow => INT_MIN
          emit_line("@" + p(4) + " div.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        } else if (di.name == "divu") {
          emit_line("mov.u32 " + r(16) + ", 0xffffffff;"); // div by zero => all-ones
          emit_line("@" + p(4) + " div.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        } else if (di.name == "rem") {
          emit_line("mov.u32 " + r(16) + ", " + r(14) + ";"); // rem by zero => dividend
          emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0;"); // overflow => 0
          emit_line("@" + p(4) + " rem.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        } else { // remu
          emit_line("mov.u32 " + r(16) + ", " + r(14) + ";"); // rem by zero => dividend
          emit_line("@" + p(4) + " rem.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
        }
      }

      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "lui" && di.imm_kind == sbt::ImmKind::U20) {
      emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }
    if (di.name == "auipc" && di.imm_kind == sbt::ImmKind::U20) {
      const uint32_t val = static_cast<uint32_t>(di.pc + static_cast<uint32_t>(di.imm));
      emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(val) + ";");
      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }
    if (di.name == "slli" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "shl.b32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if ((di.name == "srli" || di.name == "srai") && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      if (di.name == "srli") emit_line(scalar_prefix() + "shr.u32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      else emit_line(scalar_prefix() + "shr.s32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.name == "sll") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 31;");
      emit_line(scalar_prefix() + "shl.b32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "srl" || di.name == "sra") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 31;");
      if (di.name == "srl") emit_line(scalar_prefix() + "shr.u32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
      else emit_line(scalar_prefix() + "shr.s32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "xori" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "xor.b32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.name == "slt") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "sltu") {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.name == "slti" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "setp.lt.s32 " + p(1) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(15) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.name == "sltiu" && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      const uint32_t imm_u = static_cast<uint32_t>(di.imm);
      emit_line(scalar_prefix() + "setp.lt.u32 " + p(1) + ", " + r(14) + ", " + hex_u32(imm_u) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(15) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }

    // Vector loads/stores.
    // NOTE: `vlw.v` / `vsw.v` are Ventus GPU-private-memory indexed ops (Spike: insns/vlw_v.h, vsw_v.h).
    if (di.name == "vlw_v" && di.imm_kind == sbt::ImmKind::I12) {
      // base_addr = vs1 + simm11
      emit_line("add.s32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
      // offset = ((base_addr & ~3) * 32) + laneid*4
      emit_line("and.b32 " + r(15) + ", " + r(14) + ", 0xfffffffc;"); // align down to 4
      emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 5;");
      emit_line("shl.b32 " + r(16) + ", " + r(0) + ", 2;");
      emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + r(16) + ";");

      // Compute CSR_PDS (warp base) using software-stack formula.
      // blk_linear = ctaid.x + nctaid.x * (ctaid.y + nctaid.y * ctaid.z)
      emit_line("mov.u32 " + r(16) + ", %ctaid.x;");
      emit_line("mov.u32 " + r(17) + ", %ctaid.y;");
      emit_line("mov.u32 " + r(18) + ", %ctaid.z;");
      emit_line("mov.u32 " + r(19) + ", %nctaid.x;");
      emit_line("mov.u32 " + r(20) + ", %nctaid.y;");
      emit_line("mul.lo.u32 " + r(21) + ", " + r(20) + ", " + r(18) + ";");
      emit_line("add.u32 " + r(21) + ", " + r(21) + ", " + r(17) + ";");
      emit_line("mul.lo.u32 " + r(21) + ", " + r(19) + ", " + r(21) + ";");
      emit_line("add.u32 " + r(21) + ", " + r(21) + ", " + r(16) + ";"); // blk_linear

      // warp_linear = blk_linear*warps_per_block + warp_id_in_block
      emit_line("mul.lo.u32 " + r(22) + ", " + r(21) + ", " + r(12) + ";");
      emit_line("add.u32 " + r(22) + ", " + r(22) + ", " + r(10) + ";");

      // bytes_per_warp = 32*pds_size_per_thread
      emit_line("shl.b32 " + r(23) + ", " + r(29) + ", 5;");

      // warp_base = pds_base_vaddr + warp_linear*bytes_per_warp
      emit_line("cvt.u64.u32 " + rd(16) + ", " + r(22) + ";");
      emit_line("cvt.u64.u32 " + rd(17) + ", " + r(23) + ";");
      emit_line("mul.lo.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("cvt.u64.u32 " + rd(17) + ", " + r(28) + ";");
      emit_line("add.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("cvt.u32.u64 " + r(24) + ", " + rd(16) + ";"); // CSR_PDS (u32)

      // addr = CSR_PDS + offset
      emit_line("add.u32 " + r(14) + ", " + r(24) + ", " + r(15) + ";");

      emit_addr_map_and_ld_u32(r(20), r(14), pc);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(20) + ";");
      return;
    }
    if (di.name == "vsw_v" && di.imm_kind == sbt::ImmKind::S12) {
      emit_line("add.s32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
      emit_line("and.b32 " + r(15) + ", " + r(14) + ", 0xfffffffc;");
      emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 5;");
      emit_line("shl.b32 " + r(16) + ", " + r(0) + ", 2;");
      emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + r(16) + ";");

      emit_line("mov.u32 " + r(16) + ", %ctaid.x;");
      emit_line("mov.u32 " + r(17) + ", %ctaid.y;");
      emit_line("mov.u32 " + r(18) + ", %ctaid.z;");
      emit_line("mov.u32 " + r(19) + ", %nctaid.x;");
      emit_line("mov.u32 " + r(20) + ", %nctaid.y;");
      emit_line("mul.lo.u32 " + r(21) + ", " + r(20) + ", " + r(18) + ";");
      emit_line("add.u32 " + r(21) + ", " + r(21) + ", " + r(17) + ";");
      emit_line("mul.lo.u32 " + r(21) + ", " + r(19) + ", " + r(21) + ";");
      emit_line("add.u32 " + r(21) + ", " + r(21) + ", " + r(16) + ";"); // blk_linear

      emit_line("mul.lo.u32 " + r(22) + ", " + r(21) + ", " + r(12) + ";");
      emit_line("add.u32 " + r(22) + ", " + r(22) + ", " + r(10) + ";");
      emit_line("shl.b32 " + r(23) + ", " + r(29) + ", 5;");
      emit_line("cvt.u64.u32 " + rd(16) + ", " + r(22) + ";");
      emit_line("cvt.u64.u32 " + rd(17) + ", " + r(23) + ";");
      emit_line("mul.lo.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("cvt.u64.u32 " + rd(17) + ", " + r(28) + ";");
      emit_line("add.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("cvt.u32.u64 " + r(24) + ", " + rd(16) + ";"); // CSR_PDS

      emit_line("add.u32 " + r(14) + ", " + r(24) + ", " + r(15) + ";");
      emit_addr_map_and_st_u32(r(14), v(di.rs2), pc);
      return;
    }

    if (is_vector_load(di.name) && di.imm_kind == sbt::ImmKind::I12) {
      emit_line("add.u32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";"); // addr32
      if (di.name == "vlb12_v") {
        emit_addr_map_and_ld_u8_zext_u32(r(15), r(14), pc);
        emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 24;");
        emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 24;");
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else if (di.name == "vlbu12_v") {
        emit_addr_map_and_ld_u8_zext_u32(r(15), r(14), pc);
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else if (di.name == "vlh12_v") {
        emit_addr_map_and_ld_u16_zext_u32(r(15), r(14), pc);
        emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 16;");
        emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 16;");
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else if (di.name == "vlhu12_v") {
        emit_addr_map_and_ld_u16_zext_u32(r(15), r(14), pc);
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else {
        emit_addr_map_and_ld_u32(r(15), r(14), pc);
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      }
      return;
    }

    if (is_vector_store(di.name) && di.imm_kind == sbt::ImmKind::S12) {
      emit_line("add.u32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";"); // addr32
      if (di.name == "vsb12_v") {
        emit_line("cvt.u8.u32 " + u8(1) + ", " + v(di.rs2) + ";");
        emit_addr_map_and_st_u8(r(14), u8(1), pc);
      } else if (di.name == "vsh12_v") {
        emit_line("cvt.u16.u32 " + u16(1) + ", " + v(di.rs2) + ";");
        emit_addr_map_and_st_u16(r(14), u16(1), pc);
      } else {
        emit_addr_map_and_st_u32(r(14), v(di.rs2), pc);
      }
      return;
    }

    // Vector register ops.
    if (di.name == "vmv_v_x") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmv_s_x") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vfmv_v_f") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmv_v_i") {
      emit_line("mov.u32 " + v(di.rd) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vmv_v_v") {
      emit_line("mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.name == "vmv_x_s") {
      // To avoid races on the shared scalar regfile, define vmv.x.s as leader-only.
      emit_line("@" + p(0) + " mov.u32 " + r(14) + ", " + v(di.rs2) + ";");
      emit_st_x_u32_leader(di.rd, r(14), pc);
      emit_warp_sync();
      return;
    }
    if (di.name == "vid_v") {
      emit_line("mov.u32 " + v(di.rd) + ", " + r(0) + ";");
      return;
    }
    if (di.name == "vmerge_vvm") {
      // Use v0 as a per-lane boolean mask (non-zero => true).
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }
    if (di.name == "vmerge_vxm") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }
    if (di.name == "vmerge_vim") {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + std::to_string(di.imm) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }
    if (di.name == "vfmerge_vfm") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }

    if (di.name == "vadd_vv") {
      emit_line("add.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vadd_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("add.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vadd_vi") {
      emit_line("add.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vadd12_vi" && di.imm_kind == sbt::ImmKind::I12) {
      emit_line("add.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vsub_vv") {
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vsub_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vrsub_vi") {
      emit_line("sub.s32 " + v(di.rd) + ", " + std::to_string(di.imm) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.name == "vrsub_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("sub.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.name == "vminu_vv") {
      emit_line("min.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vminu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("min.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmin_vv") {
      emit_line("min.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmin_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("min.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmaxu_vv") {
      emit_line("max.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmaxu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("max.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmax_vv") {
      emit_line("max.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmax_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("max.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsub12_vi" && di.imm_kind == sbt::ImmKind::I12) {
      emit_line("sub.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vand_vv") {
      emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vand_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vand_vi") {
      emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      return;
    }
    if (di.name == "vor_vv") {
      emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vor_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vor_vi") {
      emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      return;
    }
    if (di.name == "vxor_vv") {
      emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vxor_vi") {
      emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vxor_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsll_vi") {
      emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vsll_vv") {
      emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
      emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsll_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
      emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsrl_vi") {
      emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vsrl_vv") {
      emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
      emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsrl_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
      emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsra_vi") {
      emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.name == "vsra_vv") {
      emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
      emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vsra_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
      emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmul_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.lo.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmul_vv") {
      emit_line("mul.lo.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmulh_vx") {
      // Spike semantics (vmulh vd,vs2,rs1): high half of signed multiplication.
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.hi.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmulh_vv") {
      emit_line("mul.hi.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmulhu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.hi.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vmulhu_vv") {
      emit_line("mul.hi.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmulhsu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("cvt.s64.s32 " + rd(16) + ", " + v(di.rs2) + ";");
      emit_line("cvt.s64.u32 " + rd(17) + ", " + r(14) + ";");
      emit_line("mul.lo.s64 " + rd(18) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
      emit_line("cvt.u32.s64 " + r(15) + ", " + rd(18) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      return;
    }
    if (di.name == "vmulhsu_vv") {
      emit_line("cvt.s64.s32 " + rd(16) + ", " + v(di.rs2) + ";");
      emit_line("cvt.s64.u32 " + rd(17) + ", " + v(di.rs1) + ";");
      emit_line("mul.lo.s64 " + rd(18) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
      emit_line("cvt.u32.s64 " + r(15) + ", " + rd(18) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      return;
    }
    if (di.name == "vdivu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
      emit_line("@!" + p(1) + " div.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vdivu_vv") {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
      emit_line("@!" + p(1) + " div.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vremu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      emit_line("@!" + p(1) + " rem.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vremu_vv") {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      emit_line("@!" + p(1) + " rem.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vdiv_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
      emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
      emit_line("setp.eq.u32 " + p(3) + ", " + r(14) + ", 0xffffffff;");
      emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
      emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
      emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0x80000000;");
      emit_line("@!" + p(4) + " div.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vdiv_vv") {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
      emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
      emit_line("setp.eq.u32 " + p(3) + ", " + v(di.rs1) + ", 0xffffffff;");
      emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
      emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
      emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0x80000000;");
      emit_line("@!" + p(4) + " div.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vrem_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
      emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
      emit_line("setp.eq.u32 " + p(3) + ", " + r(14) + ", 0xffffffff;");
      emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
      emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0;");
      emit_line("@!" + p(4) + " rem.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vrem_vv") {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
      emit_line("setp.eq.u32 " + p(2) + ", " + v(di.rs2) + ", 0x80000000;");
      emit_line("setp.eq.u32 " + p(3) + ", " + v(di.rs1) + ", 0xffffffff;");
      emit_line("and.pred " + p(2) + ", " + p(2) + ", " + p(3) + ";");
      emit_line("or.pred " + p(4) + ", " + p(1) + ", " + p(2) + ";");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      emit_line("@" + p(2) + " mov.u32 " + v(di.rd) + ", 0;");
      emit_line("@!" + p(4) + " rem.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.name == "vmadd_vv") {
      // RISC-V V: vmadd vd,vs1,vs2 => vd = (vd * vs1) + vs2
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.name == "vmadd_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      // RISC-V V: vmadd vd,rs1,vs2 => vd = (vd * rs1) + vs2
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.name == "vnmsub_vv") {
      // RISC-V V: vnmsub vd,vs2,vs1 (vd is also the accumulator)
      // Spike: vd = -(vd * vs1) + vs2.
      emit_line("mul.lo.u32 " + r(14) + ", " + v(di.rd) + ", " + v(di.rs1) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vnmsub_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.lo.u32 " + r(15) + ", " + v(di.rd) + ", " + r(14) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(15) + ";");
      return;
    }
    if (di.name == "vmacc_vv") {
      // Spike: vd = (vs1 * vs2) + vd
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      return;
    }
    if (di.name == "vmacc_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      return;
    }
    if (di.name == "vnmsac_vv") {
      emit_line("mul.lo.s32 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vnmsac_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.lo.s32 " + r(15) + ", " + r(14) + ", " + v(di.rs2) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(15) + ";");
      return;
    }

    // Vector compare -> 0/1 mask (treat mask registers as u32).
    auto emit_mask_from_pred = [&](const std::string &pred) { emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + pred + ";"); };

    if (di.name == "vmseq_vv") {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmseq_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmseq_vi") {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsne_vv") {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsne_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsne_vi") {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsltu_vv") {
      emit_line("setp.lt.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmslt_vv") {
      emit_line("setp.lt.s32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsleu_vv") {
      emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsleu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsleu_vi") {
      emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsle_vv") {
      emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsle_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsgtu_vi") {
      emit_line("setp.gt.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsgtu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.gt.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsgt_vi") {
      emit_line("setp.gt.s32 " + p(1) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmsgt_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.gt.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }

    // Mask boolean ops (treat non-zero as true, produce 0/1).
    if (di.name == "vmand_mm" || di.name == "vmandn_mm" || di.name == "vmor_mm" || di.name == "vmorn_mm" || di.name == "vmxor_mm" ||
        di.name == "vmxnor_mm" || di.name == "vmnand_mm" || di.name == "vmnor_mm") {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", 0;");
      emit_line("setp.ne.u32 " + p(2) + ", " + v(di.rs1) + ", 0;");

      const bool invert_b = (di.name == "vmandn_mm" || di.name == "vmorn_mm");
      if (invert_b) emit_line("not.pred " + p(2) + ", " + p(2) + ";");

      if (di.name == "vmand_mm" || di.name == "vmandn_mm") emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
      else if (di.name == "vmor_mm" || di.name == "vmorn_mm") emit_line("or.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
      else emit_line("xor.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // xor/xnor

      if (di.name == "vmxnor_mm" || di.name == "vmnand_mm" || di.name == "vmnor_mm") emit_line("not.pred " + p(3) + ", " + p(3) + ";");
      emit_mask_from_pred(p(3));
      return;
    }

    // Float compares -> 0/1 mask.
    auto emit_vf_cmp_vv = [&](const char *cmp) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs1) + ";");
      emit_line(std::string(cmp) + " " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_mask_from_pred(p(1));
    };
    auto emit_vf_cmp_vf = [&](const char *cmp) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + f(1) + ", " + r(14) + ";");
      emit_line(std::string(cmp) + " " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_mask_from_pred(p(1));
    };
    if (di.name == "vmfeq_vv") { emit_vf_cmp_vv("setp.eq.f32"); return; }
    if (di.name == "vmfeq_vf") { emit_vf_cmp_vf("setp.eq.f32"); return; }
    if (di.name == "vmfle_vv") { emit_vf_cmp_vv("setp.le.f32"); return; }
    if (di.name == "vmfle_vf") { emit_vf_cmp_vf("setp.le.f32"); return; }
    if (di.name == "vmflt_vv") { emit_vf_cmp_vv("setp.lt.f32"); return; }
    if (di.name == "vmflt_vf") { emit_vf_cmp_vf("setp.lt.f32"); return; }
    if (di.name == "vmfgt_vf") {
      // Spike: res = (rs1 < vs2)
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("setp.lt.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmfge_vf") {
      // Spike: res = (rs1 <= vs2)
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("setp.le.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmfne_vv") {
      emit_vf_cmp_vv("setp.eq.f32");
      emit_line("not.pred " + p(1) + ", " + p(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmfne_vf") {
      emit_vf_cmp_vf("setp.eq.f32");
      emit_line("not.pred " + p(1) + ", " + p(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.name == "vmslt_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.lt.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      // Spike semantics produce 0/1 (not 0xffffffff/0). Many kernels use `vxor.vi 1` to invert.
      emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
      return;
    }
    if (di.name == "vmsltu_vx") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.lt.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
      return;
    }
    if (di.name == "vmsle_vi") {
      emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
      return;
    }
    if (di.name == "vfcvt_f_x_v") {
      emit_line("cvt.rn.f32.s32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
      return;
    }
    if (di.name == "vfcvt_f_xu_v") {
      emit_line("cvt.rn.f32.u32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
      return;
    }
	    if (di.name == "vfcvt_x_f_v") {
	      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
	      emit_line("cvt.rni.s32.f32 " + r(14) + ", " + f(0) + ";");
	      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
	      return;
	    }
	    if (di.name == "vfcvt_xu_f_v") {
	      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
	      emit_line("cvt.rni.u32.f32 " + r(14) + ", " + f(0) + ";");
	      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
	      return;
	    }
    if (di.name == "vfcvt_rtz_x_f_v") {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rzi.s32.f32 " + r(14) + ", " + f(0) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vfcvt_rtz_xu_f_v") {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rzi.u32.f32 " + r(14) + ", " + f(0) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vfclass_v") {
      // RISC-V VFCLASS: return a 10-bit class mask in the destination element.
      // Bits: 0..9 = -inf, -norm, -subnorm, -0, +0, +subnorm, +norm, +inf, sNaN, qNaN
      emit_line("mov.u32 " + r(14) + ", " + v(di.rs2) + ";");
      emit_line("and.b32 " + r(15) + ", " + r(14) + ", 0x80000000;"); // sign bit
      emit_line("and.b32 " + r(16) + ", " + r(14) + ", 0x7f800000;"); // exp bits
      emit_line("and.b32 " + r(17) + ", " + r(14) + ", 0x007fffff;"); // frac bits

      emit_line("setp.ne.u32 " + p(1) + ", " + r(15) + ", 0;");          // sign
      emit_line("setp.eq.u32 " + p(2) + ", " + r(16) + ", 0;");          // exp==0
      emit_line("setp.eq.u32 " + p(3) + ", " + r(16) + ", 0x7f800000;"); // exp==all1
      emit_line("setp.eq.u32 " + p(4) + ", " + r(17) + ", 0;");          // frac==0

      // is_zero = exp==0 && frac==0
      emit_line("and.pred " + p(5) + ", " + p(2) + ", " + p(4) + ";");
      // is_sub = exp==0 && frac!=0
      emit_line("not.pred " + p(6) + ", " + p(4) + ";");
      emit_line("and.pred " + p(6) + ", " + p(2) + ", " + p(6) + ";");
      // is_inf = exp==all1 && frac==0
      emit_line("and.pred " + p(7) + ", " + p(3) + ", " + p(4) + ";");
      // is_nan = exp==all1 && frac!=0
      emit_line("not.pred " + p(8) + ", " + p(4) + ";");
      emit_line("and.pred " + p(8) + ", " + p(3) + ", " + p(8) + ";");
      // is_norm = !exp==0 && !exp==all1
      emit_line("not.pred " + p(9) + ", " + p(2) + ";");
      emit_line("not.pred " + p(10) + ", " + p(3) + ";");
      emit_line("and.pred " + p(9) + ", " + p(9) + ", " + p(10) + ";");

      // qnan = is_nan && (frac[22]==1)
      emit_line("and.b32 " + r(18) + ", " + r(17) + ", 0x00400000;");
      emit_line("setp.ne.u32 " + p(10) + ", " + r(18) + ", 0;");
      emit_line("and.pred " + p(10) + ", " + p(8) + ", " + p(10) + ";"); // qnan
      // snan = is_nan && !qnan
      emit_line("not.pred " + p(11) + ", " + p(10) + ";");
      emit_line("and.pred " + p(11) + ", " + p(8) + ", " + p(11) + ";"); // snan

      emit_line("mov.u32 " + r(19) + ", 0;");

      // -inf / +inf
      emit_line("and.pred " + p(12) + ", " + p(7) + ", " + p(1) + ";"); // -inf
      emit_line("not.pred " + p(13) + ", " + p(1) + ";");
      emit_line("and.pred " + p(13) + ", " + p(7) + ", " + p(13) + ";"); // +inf
      emit_line("@" + p(12) + " or.b32 " + r(19) + ", " + r(19) + ", 1;");
      emit_line("@" + p(13) + " or.b32 " + r(19) + ", " + r(19) + ", 128;");

      // -0 / +0
      emit_line("and.pred " + p(14) + ", " + p(5) + ", " + p(1) + ";"); // -0
      emit_line("not.pred " + p(15) + ", " + p(1) + ";");
      emit_line("and.pred " + p(15) + ", " + p(5) + ", " + p(15) + ";"); // +0
      emit_line("@" + p(14) + " or.b32 " + r(19) + ", " + r(19) + ", 8;");
      emit_line("@" + p(15) + " or.b32 " + r(19) + ", " + r(19) + ", 16;");

      // -sub / +sub
      emit_line("and.pred " + p(12) + ", " + p(6) + ", " + p(1) + ";"); // -sub
      emit_line("not.pred " + p(13) + ", " + p(1) + ";");
      emit_line("and.pred " + p(13) + ", " + p(6) + ", " + p(13) + ";"); // +sub
      emit_line("@" + p(12) + " or.b32 " + r(19) + ", " + r(19) + ", 4;");
      emit_line("@" + p(13) + " or.b32 " + r(19) + ", " + r(19) + ", 32;");

      // -norm / +norm
      emit_line("and.pred " + p(12) + ", " + p(9) + ", " + p(1) + ";"); // -norm
      emit_line("not.pred " + p(13) + ", " + p(1) + ";");
      emit_line("and.pred " + p(13) + ", " + p(9) + ", " + p(13) + ";"); // +norm
      emit_line("@" + p(12) + " or.b32 " + r(19) + ", " + r(19) + ", 2;");
      emit_line("@" + p(13) + " or.b32 " + r(19) + ", " + r(19) + ", 64;");

      // NaNs
      emit_line("@" + p(11) + " or.b32 " + r(19) + ", " + r(19) + ", 256;");
      emit_line("@" + p(10) + " or.b32 " + r(19) + ", " + r(19) + ", 512;");

      emit_line("mov.u32 " + v(di.rd) + ", " + r(19) + ";");
      return;
    }
	    if (di.name == "vfexp_v") {
	      // exp(x) ≈ 2^(x * log2(e)).
	      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
	      // PTX decimal float immediates must not use C-style `f` suffix.
	      emit_line("mul.rn.f32 " + f(1) + ", " + f(0) + ", 1.4426950408889634;");
	      emit_line("ex2.approx.f32 " + f(2) + ", " + f(1) + ";");
	      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
	      return;
	    }

    // Float vector ops: treat v regs as f32 bits.
    auto vf_binop = [&](const char *op) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs1) + ";");
      emit_line(std::string(op) + " " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
    };
    auto vf_binop_vf = [&](const char *op) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + f(1) + ", " + r(14) + ";");
      emit_line(std::string(op) + " " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
    };

    if (di.name == "vfadd_vv") { vf_binop("add.rn.f32"); return; }
    if (di.name == "vfadd_vf") { vf_binop_vf("add.rn.f32"); return; }
    if (di.name == "vfsub_vv") { vf_binop("sub.rn.f32"); return; }
    if (di.name == "vfsub_vf") { vf_binop_vf("sub.rn.f32"); return; }
    if (di.name == "vfmul_vv") { vf_binop("mul.rn.f32"); return; }
    if (di.name == "vfmul_vf") { vf_binop_vf("mul.rn.f32"); return; }
    if (di.name == "vfdiv_vv") { vf_binop("div.rn.f32"); return; }
    if (di.name == "vfdiv_vf") { vf_binop_vf("div.rn.f32"); return; }
    if (di.name == "vfrsub_vf") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("sub.rn.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
      return;
    }
    if (di.name == "vfrdiv_vf") {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("div.rn.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
      return;
    }
    if (di.name == "vfmin_vv") { vf_binop("min.f32"); return; }
    if (di.name == "vfmin_vf") { vf_binop_vf("min.f32"); return; }
    if (di.name == "vfmax_vv") { vf_binop("max.f32"); return; }
    if (di.name == "vfmax_vf") { vf_binop_vf("max.f32"); return; }
    if (di.name == "vfmadd_vv") {
      // Spike semantics: vfmadd.vv vd,vs1,vs2 => vd = (vd * vs1) + vs2
      emit_line("mov.b32 " + f(0) + ", " + v(di.rd) + ";");  // old vd
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs1) + ";"); // vs1
      emit_line("mov.b32 " + f(2) + ", " + v(di.rs2) + ";"); // vs2
      emit_line("fma.rn.f32 " + f(3) + ", " + f(0) + ", " + f(1) + ", " + f(2) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(3) + ";");
      return;
    }
    auto vf_fma_bits = [&](const std::string &a_bits, bool neg_a, const std::string &b_bits, bool neg_b, const std::string &c_bits, bool neg_c) {
      emit_line("mov.u32 " + r(14) + ", " + a_bits + ";");
      emit_line("mov.u32 " + r(15) + ", " + b_bits + ";");
      emit_line("mov.u32 " + r(16) + ", " + c_bits + ";");
      if (neg_a) emit_line("xor.b32 " + r(14) + ", " + r(14) + ", 0x80000000;");
      if (neg_b) emit_line("xor.b32 " + r(15) + ", " + r(15) + ", 0x80000000;");
      if (neg_c) emit_line("xor.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + r(15) + ";");
      emit_line("mov.b32 " + f(2) + ", " + r(16) + ";");
      emit_line("fma.rn.f32 " + f(3) + ", " + f(0) + ", " + f(1) + ", " + f(2) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(3) + ";");
    };
    if (di.name == "vfmadd_vf") {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), false, r(15), false, v(di.rs2), false);
      return;
    }
    if (di.name == "vfmsub_vv") {
      vf_fma_bits(v(di.rd), false, v(di.rs1), false, v(di.rs2), true);
      return;
    }
    if (di.name == "vfmsub_vf") {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), false, r(15), false, v(di.rs2), true);
      return;
    }
    if (di.name == "vfnmadd_vv") {
      vf_fma_bits(v(di.rd), true, v(di.rs1), false, v(di.rs2), true);
      return;
    }
    if (di.name == "vfnmadd_vf") {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), true, r(15), false, v(di.rs2), true);
      return;
    }
    if (di.name == "vfnmsub_vv") {
      vf_fma_bits(v(di.rd), true, v(di.rs1), false, v(di.rs2), false);
      return;
    }
    if (di.name == "vfnmsub_vf") {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), true, r(15), false, v(di.rs2), false);
      return;
    }
    if (di.name == "vfmacc_vv") {
      // Spike: vd = (vs1 * vs2) + vd
      vf_fma_bits(v(di.rs1), false, v(di.rs2), false, v(di.rd), false);
      return;
    }
    if (di.name == "vfmacc_vf") {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), false, v(di.rd), false);
      return;
    }
    if (di.name == "vfnmacc_vv") {
      // Spike: vd = -(vs1 * vs2) - vd  == fma(-vs2, vs1, -vd)
      vf_fma_bits(v(di.rs2), true, v(di.rs1), false, v(di.rd), true);
      return;
    }
    if (di.name == "vfnmacc_vf") {
      // Spike: vd = -(rs1 * vs2) - vd == fma(rs1, -vs2, -vd)
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), true, v(di.rd), true);
      return;
    }
    if (di.name == "vfmsac_vv") {
      // Spike: vd = (vs1 * vs2) - vd == fma(vs1, vs2, -vd)
      vf_fma_bits(v(di.rs1), false, v(di.rs2), false, v(di.rd), true);
      return;
    }
    if (di.name == "vfmsac_vf") {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), false, v(di.rd), true);
      return;
    }
    if (di.name == "vfnmsac_vv") {
      // Spike: vd = -(vs2 * vs1) + vd == fma(-vs1, vs2, vd)
      vf_fma_bits(v(di.rs1), true, v(di.rs2), false, v(di.rd), false);
      return;
    }
    if (di.name == "vfnmsac_vf") {
      // Spike: vd = -(rs1 * vs2) + vd == fma(rs1, -vs2, vd)
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), true, v(di.rd), false);
      return;
    }
    if (di.name == "vfsqrt_v") {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("sqrt.rn.f32 " + f(1) + ", " + f(0) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(1) + ";");
      return;
    }
    if (di.name == "vfsgnj_vv" || di.name == "vfsgnj_vf" || di.name == "vfsgnjn_vv" || di.name == "vfsgnjn_vf" || di.name == "vfsgnjx_vv" ||
        di.name == "vfsgnjx_vf") {
      if (di.name == "vfsgnj_vf" || di.name == "vfsgnjn_vf" || di.name == "vfsgnjx_vf") {
        emit_ld_x_u32_all(r(14), di.rs1, pc);
      }
      const std::string sign_src = (di.name == "vfsgnj_vf" || di.name == "vfsgnjn_vf" || di.name == "vfsgnjx_vf") ? r(14) : v(di.rs1);
      emit_line("and.b32 " + r(15) + ", " + v(di.rs2) + ", 0x7fffffff;"); // magnitude from vs2
      if (di.name == "vfsgnj_vv" || di.name == "vfsgnj_vf") {
        emit_line("and.b32 " + r(16) + ", " + sign_src + ", 0x80000000;");
      } else if (di.name == "vfsgnjn_vv" || di.name == "vfsgnjn_vf") {
        emit_line("not.b32 " + r(16) + ", " + sign_src + ";");
        emit_line("and.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
      } else { // vfsgnjx
        emit_line("xor.b32 " + r(16) + ", " + sign_src + ", " + v(di.rs2) + ";");
        emit_line("and.b32 " + r(16) + ", " + r(16) + ", 0x80000000;");
      }
      emit_line("or.b32 " + v(di.rd) + ", " + r(15) + ", " + r(16) + ";");
      return;
    }

    throw EmitError("unsupported.inst", func_name, pc, di.name);
  }

  void emit_prologue() {
    // Params:
    //  - elf_base: backing buffer for [elf_base_vaddr, heap_base_vaddr)
    //  - heap_base: backing buffer for [heap_base_vaddr, ...)
    //  - knl_vaddr: Ventus numeric address of metadata buffer (u32)
    //  - pds_base_vaddr: Ventus numeric address of the global PDS buffer base (u32)
    //  - pds_size_per_thread: bytes of private memory per thread (u32)
    emit_raw(".visible .entry " + ptx_name + "(\n");
    emit_raw("    .param .u64 elf_base,\n");
    emit_raw("    .param .u64 heap_base,\n");
    emit_raw("    .param .u32 knl_vaddr,\n");
    emit_raw("    .param .u32 pds_base_vaddr,\n");
    emit_raw("    .param .u32 pds_size_per_thread\n");
    emit_raw(")\n{\n");

    emit_line(".reg .b32 %r<32>;");
    emit_line(".reg .b64 %rd<32>;");
    emit_line(".reg .pred %p<16>;");
    emit_line(".reg .f32 %f<16>;");
    emit_line(".reg .u8 %ub<4>;");
    emit_line(".reg .u16 %uh<4>;");
    emit_line(".reg .b32 %v<256>;");

    if (mod.need_vctx) {
      emit_line(".local .align 4 .b8 __sbt_vctx[1024];");
      // `mov` yields a local-space address for local symbols; use it directly with `ld/st.local`.
      emit_line("mov.u64 " + rd(6) + ", __sbt_vctx;");
    } else {
      emit_line("mov.u64 " + rd(6) + ", 0;");
    }

    // Load params and compute global/shared base pointers.
    emit_line("ld.param.u64 " + rd(10) + ", [elf_base];");
    emit_line("cvta.to.global.u64 " + rd(0) + ", " + rd(10) + ";");
    emit_line("ld.param.u64 " + rd(11) + ", [heap_base];");
    emit_line("cvta.to.global.u64 " + rd(1) + ", " + rd(11) + ";");
    emit_line("ld.param.u32 " + r(30) + ", [knl_vaddr];");
    emit_line("ld.param.u32 " + r(28) + ", [pds_base_vaddr];");
    emit_line("ld.param.u32 " + r(29) + ", [pds_size_per_thread];");

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

    // wctx_ptr = shmem_base + warp_id * 1024
    emit_line("shl.b32 " + r(13) + ", " + r(10) + ", 10;");
    emit_line("cvt.u64.u32 " + rd(13) + ", " + r(13) + ";");
    emit_line("add.u64 " + rd(3) + ", " + rd(2) + ", " + rd(13) + ";");

    // lds_ptr = shmem_base + warps_per_block * 1024
    emit_line("shl.b32 " + r(14) + ", " + r(12) + ", 10;");
    emit_line("cvt.u64.u32 " + rd(14) + ", " + r(14) + ";");
    emit_line("add.u64 " + rd(4) + ", " + rd(2) + ", " + rd(14) + ";");

    // Leader + init ABI-critical scalar regs (x2 stack, x10 argbase).
    emit_block_preamble();

    // Fail fast if pds_base_vaddr is outside heap/global numeric address range.
    // Allow pds_size_per_thread==0 to bypass the check (some kernels may not allocate private memory).
    emit_line("setp.lt.u32 " + p(1) + ", " + r(28) + ", " + hex_u32(opt.heap_base_vaddr) + ";");
    emit_line("setp.ne.u32 " + p(2) + ", " + r(29) + ", 0;");
    emit_line("and.pred " + p(1) + ", " + p(1) + ", " + p(2) + ";");
    emit_line("@" + p(1) + " trap;");

    // Match `_start` ABI: tp (x4) starts at 0 and is used as a spill-stack cursor.
    emit_line("@" + p(0) + " mov.u32 " + r(15) + ", 0;");
    emit_st_x_u32_leader(/*x4=*/4, r(15), /*pc_for_err=*/cfg.start);

    // x2 = shared_base + warp_id * stack_stride (default: 1024 bytes => <<10)
    emit_line("@" + p(0) + " shl.b32 " + r(15) + ", " + r(10) + ", 10;");
    emit_line("@" + p(0) + " add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_st_x_u32_leader(/*x2=*/2, r(15), /*pc_for_err=*/cfg.start);

    // Match `_start` ABI: s0 (x8) points to the base of the kernel LDS region:
    //   s0 = CSR_LDS + CSR_NUMW*1024
    // In this backend `shared_base_vaddr` models the CSR_LDS numeric base, and `warps_per_block` models CSR_NUMW.
    // Note: kernels may further adjust s0 in their own prologue (e.g. `addi s0, s0, <frame_bytes>`). We treat that
    // as frame allocation and do not attempt to compensate it here.
    emit_line("@" + p(0) + " shl.b32 " + r(15) + ", " + r(12) + ", 10;");
    emit_line("@" + p(0) + " add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_st_x_u32_leader(/*x8=*/8, r(15), /*pc_for_err=*/cfg.start);

    // x10 (a0) is the first argument register. PoCL Ventus kernels expect:
    //   a0 = *(u32*)(CSR_KNL + 4)  (arg buffer base)
    // because the original `_start` loads it from the hardware metadata buffer before jumping to the kernel entry.
    emit_line("@" + p(0) + " add.u32 " + r(16) + ", " + r(30) + ", 4;"); // arg_base field address
    emit_line("@" + p(0) + " add.u32 " + r(16) + ", " + r(16) + ", 0;"); // keep in u32 reg
    emit_addr_map_and_ld_u32_leader(r(17), r(16), /*pc_for_err=*/cfg.start);
    emit_st_x_u32_leader(/*x10=*/10, r(17), /*pc_for_err=*/cfg.start);

    emit_warp_sync();

    // Fallthrough to entry BB.
  }

  void emit_func_prologue() {
    // Pass-through params computed in the caller and required for address mapping / CSR reads.
    emit_raw(".func " + ptx_name + "(\n");
    emit_raw("    .param .u64 __sbt_arg_elf_base,\n");
    emit_raw("    .param .u64 __sbt_arg_heap_base,\n");
    emit_raw("    .param .u64 __sbt_arg_wctx_ptr,\n");
    emit_raw("    .param .u64 __sbt_arg_lds_ptr,\n");
    emit_raw("    .param .u32 __sbt_arg_knl_vaddr,\n");
    emit_raw("    .param .u32 __sbt_arg_pds_base_vaddr,\n");
    emit_raw("    .param .u32 __sbt_arg_pds_size_per_thread,\n");
    emit_raw("    .param .u32 __sbt_arg_warp_id,\n");
    emit_raw("    .param .u32 __sbt_arg_warps_per_block");
    if (mod.need_vctx) emit_raw(",\n    .param .u64 __sbt_arg_vctx_base");
    emit_raw("\n");
    emit_raw(")\n{\n");

    emit_line(".reg .b32 %r<32>;");
    emit_line(".reg .b64 %rd<32>;");
    emit_line(".reg .pred %p<16>;");
    emit_line(".reg .f32 %f<16>;");
    emit_line(".reg .u8 %ub<4>;");
    emit_line(".reg .u16 %uh<4>;");
    emit_line(".reg .b32 %v<256>;");

    emit_line("ld.param.u64 " + rd(0) + ", [__sbt_arg_elf_base];");
    emit_line("ld.param.u64 " + rd(1) + ", [__sbt_arg_heap_base];");
    emit_line("ld.param.u64 " + rd(3) + ", [__sbt_arg_wctx_ptr];");
    emit_line("ld.param.u64 " + rd(4) + ", [__sbt_arg_lds_ptr];");
    emit_line("ld.param.u32 " + r(30) + ", [__sbt_arg_knl_vaddr];");
    emit_line("ld.param.u32 " + r(28) + ", [__sbt_arg_pds_base_vaddr];");
    emit_line("ld.param.u32 " + r(29) + ", [__sbt_arg_pds_size_per_thread];");
    emit_line("ld.param.u32 " + r(10) + ", [__sbt_arg_warp_id];");
    emit_line("ld.param.u32 " + r(12) + ", [__sbt_arg_warps_per_block];");
    if (mod.need_vctx) emit_line("ld.param.u64 " + rd(6) + ", [__sbt_arg_vctx_base];");

    emit_line("mov.u32 " + r(0) + ", %laneid;");
    if (mod.need_vctx) {
      emit_line("// load v-regfile from vctx at function entry");
      emit_vctx_load_all(cfg.start);
    }
  }

  void emit_body() {
    if (is_entry) emit_prologue();
    else emit_func_prologue();

    // Emit blocks in address order.
    for (const auto &bb : cfg.blocks) {
      out << label_bb(bb.start) << ":\n";
      emit_block_preamble();
      for (size_t idx : bb.inst_indices) {
        emit_one_inst(cfg.insts[idx]);
      }
    }

    emit_raw("}\n\n");
  }
};

} // namespace

EmitResult emit_module(const sbt::cfg::FunctionCfg &entry_cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &entry_name, const std::vector<FuncToEmit> &funcs,
                       const std::unordered_map<uint32_t, std::string> &ptx_name_by_addr, const Options &opt) {
  // Module header.
  std::ostringstream out;
  out << ".version 7.0\n";
  out << ".target sm_" << opt.sm << "\n";
  out << ".address_size 64\n\n";
  out << ".extern .shared .align 16 .b8 __sbt_shmem[];\n\n";

  ModuleInfo mod;
  mod.ptx_name_by_addr = &ptx_name_by_addr;
  mod.need_vctx = !funcs.empty();

  // Emit `.func`s first.
  for (const auto &f : funcs) {
    EmitCtx ctx(f.cfg, sym_by_addr, f.name, f.ptx_name, opt, mod, /*is_entry=*/false);
    ctx.emit_body();
    out << ctx.out.str();
  }

  // Emit `.entry` last.
  EmitCtx entry(entry_cfg, sym_by_addr, entry_name, entry_name, opt, mod, /*is_entry=*/true);
  entry.emit_body();
  out << entry.out.str();

  EmitResult res;
  res.ptx = out.str();
  return res;
}

EmitResult emit_kernel(const sbt::cfg::FunctionCfg &cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &kernel_name, const Options &opt) {
  const std::unordered_map<uint32_t, std::string> empty;
  const std::vector<FuncToEmit> none;
  return emit_module(cfg, sym_by_addr, kernel_name, none, empty, opt);
}

} // namespace sbt::ptx
