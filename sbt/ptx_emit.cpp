#include "sbt/ptx_emit.hpp"
#include "sbt/ptx_mma.hpp"

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
static std::string x(int i) { return "%x" + std::to_string(i); }
static std::string h(int i) { return "%h" + std::to_string(i); }
static std::string u8(int i) { return "%ub" + std::to_string(i); }
static std::string u16(int i) { return "%uh" + std::to_string(i); }

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
static constexpr uint32_t kKnlArgBaseOffset = 4u;
static constexpr uint32_t kKnlPrintAddrOffset = 48u;
static constexpr uint32_t kKnlLdsStackSizePerWfOffset = 56u;

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

enum class ScalarExecKind {
  UniformPure,
  LaneSensitive,
  FixedLaneSensitive,
  ExternallySideEffecting,
};

struct ScalarExecRule final {
  std::string_view name;
  ScalarExecKind kind;
};

static constexpr std::array<ScalarExecRule, 18> kScalarExecRules{{
    {"lb", ScalarExecKind::UniformPure},
    {"lh", ScalarExecKind::UniformPure},
    {"lw", ScalarExecKind::UniformPure},
    {"lbu", ScalarExecKind::UniformPure},
    {"lhu", ScalarExecKind::UniformPure},
    {"flw", ScalarExecKind::UniformPure},
    {"sb", ScalarExecKind::ExternallySideEffecting},
    {"sh", ScalarExecKind::ExternallySideEffecting},
    {"sw", ScalarExecKind::ExternallySideEffecting},
    {"fsw", ScalarExecKind::ExternallySideEffecting},
    {"csrrw", ScalarExecKind::UniformPure},
    {"csrrs", ScalarExecKind::UniformPure},
    {"csrrc", ScalarExecKind::UniformPure},
    {"csrrwi", ScalarExecKind::UniformPure},
    {"csrrsi", ScalarExecKind::UniformPure},
    {"csrrci", ScalarExecKind::UniformPure},
    {"vmv_x_s", ScalarExecKind::FixedLaneSensitive},
    {"trap", ScalarExecKind::ExternallySideEffecting},
}};

static std::optional<ScalarExecKind> lookup_scalar_exec_kind(std::string_view name) {
  for (const auto &rule : kScalarExecRules) {
    if (rule.name == name) return rule.kind;
  }
  return std::nullopt;
}

static ScalarExecKind scalar_exec_kind_for_inst(const sbt::DecodedInst &di) {
  if (const auto kind = lookup_scalar_exec_kind(di.name)) return *kind;
  if (is_scalar_branch(di.name)) return ScalarExecKind::UniformPure;
  return ScalarExecKind::UniformPure;
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

static void emit_helper_func_signature(std::ostringstream &out, const std::string &ptx_name) {
  out << ".func (\n";
  out << "    .param .align 4 .b8 __sbt_mutable_state_out[" << kMutableStateBytes << "]\n";
  out << ") " << ptx_name << "(\n";
  out << "    .param .align 4 .b8 __sbt_mutable_state_in[" << kMutableStateBytes << "],\n";
  out << "    .param .align 4 .b8 __sbt_machine_ctx_in[" << kMachineCtxBytes << "],\n";
  out << "    .param .align 8 .b8 __sbt_runtime_env_in[" << kRuntimeEnvBytes << "]\n";
  out << ")";
}

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
  int tmp_label_id = 0;
  bool emitted_fp_dyn_note = false;
  bool control_protocol_ready = false;

  EmitCtx(const sbt::cfg::FunctionCfg &cfg_, const std::unordered_map<uint32_t, std::string> &sym_by_addr_,
          const std::string &func_name_, const std::string &ptx_name_, const Options &opt_, const ModuleInfo &mod_, bool is_entry_)
      : cfg(cfg_), sym_by_addr(sym_by_addr_), func_name(func_name_), ptx_name(ptx_name_), opt(opt_), mod(mod_), is_entry(is_entry_) {}

  // Register assignment conventions (must match PTX declarations).
  // %r0: laneid
  // %r1: use-point activemask scratch
  // %r2: persistent leader_lane
  // %p0: is_leader
  // %rd0: global_base (global)
  // %rd1: reserved scratch
  // %rd2: shmem_base (shared)
  // %rd3: reserved (legacy wctx_ptr slot)
  // %rd4: reserved shared scratch (kept to avoid register-map churn)
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

  void emit_line(const std::string &s) { out << "  " << s << "\n"; }
  void emit_raw(const std::string &s) { out << s; }
  void emit_label(const std::string &name) { out << name << ":\n"; }

  void emit_read_activemask(const std::string &dst_r) { emit_line("activemask.b32 " + dst_r + ";"); }

  void emit_refresh_leader_predicate() { emit_line("setp.eq.u32 " + p(0) + ", " + r(0) + ", " + r(2) + ";"); }

  void emit_select_leader_from_active_mask() {
    emit_read_activemask(r(1));
    emit_line("bfind.u32 " + r(2) + ", " + r(1) + ";");
    emit_refresh_leader_predicate();
  }

  void emit_warp_sync() {
    emit_read_activemask(r(1));
    emit_line("bar.warp.sync " + r(1) + ";");
  }

  void emit_trap_if_lane_inactive(uint32_t lane) {
    require(lane < 32u, EmitError("unsupported.fixed_lane", func_name, cfg.start, "lane>=32"));
    emit_read_activemask(r(1));
    emit_line("and.b32 " + r(14) + ", " + r(1) + ", " + std::to_string(1u << lane) + ";");
    emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
    emit_line("@" + p(1) + " trap;");
  }

  void emit_compute_csr_pds_u32(const std::string &dst_r, bool scalar) {
    const std::string pre = scalar ? scalar_prefix() : "";
    emit_line(pre + "ld.shared.u32 " + r(24) + ", [__sbt_pds_wg_base];");
    emit_line(pre + "shl.b32 " + r(25) + ", " + r(29) + ", 5;");
    emit_line(pre + "mul.lo.u32 " + r(23) + ", " + r(10) + ", " + r(25) + ";");
    emit_line(pre + "add.u32 " + dst_r + ", " + r(24) + ", " + r(23) + ";");
  }

  void emit_entry_pds_pool_acquire(uint32_t pc_for_err) {
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

  void emit_entry_pds_pool_release(uint32_t pc_for_err) {
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

  void prepare_control_protocol() {
    if (control_protocol_ready) return;
    control_protocol_ready = true;
    for (const auto &bb : cfg.blocks) {
      if (bb.inst_indices.empty()) continue;
      for (size_t idx : bb.inst_indices) {
        const auto &inst = cfg.insts[idx].inst;
        if (inst.name == "join" && idx != bb.inst_indices.front()) {
          throw EmitError("unsupported.join", func_name, inst.pc, "join must start a basic block");
        }
      }
    }
  }

  void emit_boundary_labels_for_block(uint32_t block_start) { (void)block_start; }

  std::string target_label_for_edge(uint32_t src_block, uint32_t dst_block) const {
    (void)src_block;
    return label_bb(dst_block);
  }

  void emit_ld_x_u32_leader(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
    if (xreg == 0) {
      emit_line("@" + p(0) + " mov.u32 " + dst_r + ", 0;");
      return;
    }
    require(xreg >= 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<0"));
    emit_line("@" + p(0) + " mov.u32 " + dst_r + ", " + x(xreg) + ";");
  }

  void emit_broadcast_from_leader(const std::string &dst_r, const std::string &src_r) {
    emit_read_activemask(r(1));
    emit_line("shfl.sync.idx.b32 " + dst_r + ", " + src_r + ", " + r(2) + ", 0x1f, " + r(1) + ";");
  }

  void emit_ld_x_u32_all(const std::string &dst_r, int xreg, uint32_t pc_for_err) {
    if (xreg == 0) {
      emit_line("mov.u32 " + dst_r + ", 0;");
      return;
    }
    require(xreg >= 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<0"));
    emit_line("mov.u32 " + dst_r + ", " + x(xreg) + ";");
  }

  void emit_st_x_u32_leader(int xreg, const std::string &src_r, uint32_t pc_for_err) {
    if (xreg == 0) return; // x0 is hard-wired zero.
    require(xreg > 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<=0"));
    emit_line("@" + p(0) + " mov.u32 " + x(xreg) + ", " + src_r + ";");
  }

  void emit_st_x_u32_all(int xreg, const std::string &src_r, uint32_t pc_for_err) {
    if (xreg == 0) return; // x0 is hard-wired zero.
    require(xreg > 0, EmitError("invalid.reg", func_name, pc_for_err, "xreg<=0"));
    emit_line("mov.u32 " + x(xreg) + ", " + src_r + ";");
  }

  std::string scalar_prefix() const { return ""; }

  void emit_store_param_u32(const std::string &base, uint32_t offset, const std::string &src_r, const std::string &prefix = "") {
    emit_line(prefix + "st.param.u32 [" + base + "+" + std::to_string(offset) + "], " + src_r + ";");
  }

  void emit_store_param_u64(const std::string &base, uint32_t offset, const std::string &src_rd) {
    emit_line("st.param.u64 [" + base + "+" + std::to_string(offset) + "], " + src_rd + ";");
  }

  void emit_load_param_u32(const std::string &dst_r, const std::string &base, uint32_t offset, const std::string &prefix = "") {
    emit_line(prefix + "ld.param.u32 " + dst_r + ", [" + base + "+" + std::to_string(offset) + "];");
  }

  void emit_load_param_u64(const std::string &dst_rd, const std::string &base, uint32_t offset) {
    emit_line("ld.param.u64 " + dst_rd + ", [" + base + "+" + std::to_string(offset) + "];");
  }

  void emit_store_mutable_state_blob(const std::string &blob_name) {
    // Call/ret boundaries must export a warp-consistent leader metadata value, not a stale path-local leader.
    emit_select_leader_from_active_mask();
    emit_store_param_u32(blob_name, kMutableLeaderOffset, r(2));
    emit_line("mov.u32 " + r(31) + ", 0;");
    emit_store_param_u32(blob_name, kMutableXOffset, r(31));
    for (uint32_t i = 1; i < kNumXRegs; ++i) {
      emit_store_param_u32(blob_name, kMutableXOffset + i * kBlobWordBytes, x(static_cast<int>(i)));
    }
    for (uint32_t i = 0; i < kNumVRegs; ++i) {
      emit_store_param_u32(blob_name, kMutableVOffset + i * kBlobWordBytes, v(static_cast<int>(i)));
    }
  }

  void emit_restore_mutable_state_blob(const std::string &blob_name) {
    emit_load_param_u32(r(2), blob_name, kMutableLeaderOffset);
    emit_refresh_leader_predicate();
    for (uint32_t i = 1; i < kNumXRegs; ++i) {
      emit_load_param_u32(x(static_cast<int>(i)), blob_name, kMutableXOffset + i * kBlobWordBytes);
    }
    for (uint32_t i = 0; i < kNumVRegs; ++i) {
      emit_load_param_u32(v(static_cast<int>(i)), blob_name, kMutableVOffset + i * kBlobWordBytes);
    }
  }

  void emit_store_runtime_env_blob(const std::string &blob_name) {
    emit_store_param_u64(blob_name, kRuntimeGlobalOffset, rd(0));
  }

  void emit_store_machine_ctx_blob(const std::string &blob_name) {
    emit_store_param_u32(blob_name, kMachineKnlOffset, r(30));
    emit_store_param_u32(blob_name, kMachinePdsBaseOffset, r(28));
    emit_store_param_u32(blob_name, kMachinePdsSizeOffset, r(29));
    emit_store_param_u32(blob_name, kMachineWarpIdOffset, r(10));
    emit_store_param_u32(blob_name, kMachineWarpsPerBlockOffset, r(12));
  }

  void emit_load_runtime_env_blob(const std::string &blob_name) {
    emit_load_param_u64(rd(0), blob_name, kRuntimeGlobalOffset);
  }

  void emit_load_machine_ctx_blob(const std::string &blob_name) {
    emit_load_param_u32(r(30), blob_name, kMachineKnlOffset);
    emit_load_param_u32(r(28), blob_name, kMachinePdsBaseOffset);
    emit_load_param_u32(r(29), blob_name, kMachinePdsSizeOffset);
    emit_load_param_u32(r(10), blob_name, kMachineWarpIdOffset);
    emit_load_param_u32(r(12), blob_name, kMachineWarpsPerBlockOffset);
  }

  void emit_load_knl_u32_scalar(const std::string &dst_r, uint32_t offset, uint32_t pc_for_err) {
    emit_line("add.u32 " + r(16) + ", " + r(30) + ", " + std::to_string(offset) + ";");
    emit_addr_map_and_ld_u32_scalar(dst_r, r(16), pc_for_err);
  }

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
    emit_ld_x_u32_all(dst_r, xreg, pc_for_err);
  }

  void emit_st_x_u32_scalar(int xreg, const std::string &src_r, uint32_t pc_for_err) {
    emit_st_x_u32_all(xreg, src_r, pc_for_err);
  }

  void emit_addr_map_and_ld_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
  }

  void emit_addr_map_and_st_u32_scalar(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    emit_select_leader_from_active_mask();
    emit_addr_map_and_st_u32_leader(addr_r, src_r, pc_for_err);
    emit_warp_sync();
  }

  void emit_addr_map_and_ld_u8_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_addr_map_and_ld_u8_zext_u32(dst_r, addr_r, pc_for_err);
  }

  void emit_addr_map_and_ld_u16_zext_u32_scalar(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_addr_map_and_ld_u16_zext_u32(dst_r, addr_r, pc_for_err);
  }

  void emit_addr_map_and_st_u8_scalar(const std::string &addr_r, const std::string &src_u8, uint32_t pc_for_err) {
    emit_select_leader_from_active_mask();
    emit_addr_map_and_st_u8_leader(addr_r, src_u8, pc_for_err);
    emit_warp_sync();
  }

  void emit_addr_map_and_st_u16_scalar(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
    emit_select_leader_from_active_mask();
    emit_addr_map_and_st_u16_leader(addr_r, src_u16, pc_for_err);
    emit_warp_sync();
  }

  void emit_prepare_addr_mapping(const std::string &addr_r, uint32_t pc_for_err) {
    (void)pc_for_err;

    // Copy to a scratch register to avoid aliasing with r16 temporaries (addr_r can be %r16 in prologue).
    emit_line("mov.u32 " + r(31) + ", " + addr_r + ";");

    // Shared addresses live in [shared_base_vaddr, global_base_vaddr); everything below shared is invalid.
    emit_line("setp.ge.u32 " + p(1) + ", " + r(31) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("setp.lt.u32 " + p(2) + ", " + r(31) + ", " + hex_u32(opt.global_base_vaddr) + ";");
    emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";"); // p3 = is_shared
    emit_line("setp.ge.u32 " + p(4) + ", " + r(31) + ", " + hex_u32(opt.global_base_vaddr) + ";"); // p4 = is_global
    emit_line("or.pred " + p(5) + ", " + p(3) + ", " + p(4) + ";");
    emit_line("@!" + p(5) + " trap;");

    // shared_ptr -> rd17
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.shared_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(17) + ", " + rd(2) + ", " + rd(16) + ";");

    // global_ptr -> rd16
    emit_line("add.u32 " + r(16) + ", " + r(31) + ", -" + hex_u32(opt.global_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(16) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(0) + ", " + rd(16) + ";");
  }

  void emit_addr_map_and_ld_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + p(3) + " ld.shared.u32 " + dst_r + ", [" + rd(17) + "];");
    emit_line("@" + p(4) + " ld.global.u32 " + dst_r + ", [" + rd(16) + "];");
  }

  void emit_addr_map_and_ld_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_ld32_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_st_u32(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + p(3) + " st.shared.u32 [" + rd(17) + "], " + src_r + ";");
    emit_line("@" + p(4) + " st.global.u32 [" + rd(16) + "], " + src_r + ";");
  }

  void emit_addr_map_and_st_u32_leader(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_st32_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_st_u32(addr_r, src_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_ld_u8_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + p(3) + " ld.shared.u8 " + u8(0) + ", [" + rd(17) + "];");
    emit_line("@" + p(4) + " ld.global.u8 " + u8(0) + ", [" + rd(16) + "];");
    emit_line("cvt.u32.u8 " + dst_r + ", " + u8(0) + ";");
  }

  void emit_addr_map_and_ld_u16_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + p(3) + " ld.shared.u16 " + u16(0) + ", [" + rd(17) + "];");
    emit_line("@" + p(4) + " ld.global.u16 " + u16(0) + ", [" + rd(16) + "];");
    emit_line("cvt.u32.u16 " + dst_r + ", " + u16(0) + ";");
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
    emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + p(3) + " st.shared.u8 [" + rd(17) + "], " + src_u8 + ";");
    emit_line("@" + p(4) + " st.global.u8 [" + rd(16) + "], " + src_u8 + ";");
  }

  void emit_addr_map_and_st_u16(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
    emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + p(3) + " st.shared.u16 [" + rd(17) + "], " + src_u16 + ";");
    emit_line("@" + p(4) + " st.global.u16 [" + rd(16) + "], " + src_u16 + ";");
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
    const std::string p_mut_in = "__sbt_call_mutable_in_" + std::to_string(tmp_label_id++);
    const std::string p_mut_out = "__sbt_call_mutable_out_" + std::to_string(tmp_label_id++);
    const std::string p_machine = "__sbt_call_machine_" + std::to_string(tmp_label_id++);
    const std::string p_runtime = "__sbt_call_runtime_" + std::to_string(tmp_label_id++);

    emit_line(".param .align 4 .b8 " + p_mut_in + "[" + std::to_string(kMutableStateBytes) + "];");
    emit_line(".param .align 4 .b8 " + p_mut_out + "[" + std::to_string(kMutableStateBytes) + "];");
    emit_line(".param .align 4 .b8 " + p_machine + "[" + std::to_string(kMachineCtxBytes) + "];");
    emit_line(".param .align 8 .b8 " + p_runtime + "[" + std::to_string(kRuntimeEnvBytes) + "];");

    emit_store_mutable_state_blob(p_mut_in);
    emit_store_machine_ctx_blob(p_machine);
    emit_store_runtime_env_blob(p_runtime);
    emit_line("call.uni (" + p_mut_out + "), " + callee_ptx + ", (" + p_mut_in + ", " + p_machine + ", " + p_runtime + ");");
    emit_restore_mutable_state_blob(p_mut_out);
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

  std::string mma_detail(const sbt::MmaInstInfo &mma) const {
    return "shape=" + std::string(sbt::to_string(mma.shape)) + " layout=" + std::string(sbt::to_string(mma.a_layout)) + "." +
           std::string(sbt::to_string(mma.b_layout)) + " ab=" + std::string(sbt::to_string(mma.ab_type)) + " cd=" +
           std::string(sbt::to_string(mma.cd_type)) + " support=" + std::string(sbt::to_string(mma.support_class)) + " lowering=" +
           std::string(sbt::to_string(mma.lowering_class));
  }

  void emit_compute_mma_scratch_base(const std::string &dst_rd, uint32_t pc_for_err) {
    emit_load_knl_u32_scalar(r(14), kKnlLdsStackSizePerWfOffset, pc_for_err);
    emit_line("setp.lt.u32 " + p(1) + ", " + r(14) + ", 1024;");
    emit_line("@" + p(1) + " trap;");
    emit_line("mul.lo.u32 " + r(15) + ", " + r(10) + ", " + r(14) + ";");
    emit_line("cvt.u64.u32 " + rd(19) + ", " + r(15) + ";");
    emit_line("add.u64 " + dst_rd + ", " + rd(2) + ", " + rd(19) + ";");
  }

  void emit_load_u32_from_mma_scratch(const std::string &dst_r, const std::string &scratch_rd, const std::string &reg_r,
                                      const std::string &lane_r, const std::string &off_r) {
    emit_line("mul.lo.u32 " + off_r + ", " + reg_r + ", 128;");
    emit_line("shl.b32 " + r(21) + ", " + lane_r + ", 2;");
    emit_line("add.u32 " + off_r + ", " + off_r + ", " + r(21) + ";");
    emit_line("cvt.u64.u32 " + rd(19) + ", " + off_r + ";");
    emit_line("add.u64 " + rd(19) + ", " + scratch_rd + ", " + rd(19) + ";");
    emit_line("ld.shared.u32 " + dst_r + ", [" + rd(19) + "];");
  }

  void emit_store_u32_to_mma_scratch(const std::string &scratch_rd, const std::string &reg_r, const std::string &lane_r,
                                     const std::string &src_r, const std::string &off_r) {
    emit_line("mul.lo.u32 " + off_r + ", " + reg_r + ", 128;");
    emit_line("shl.b32 " + r(21) + ", " + lane_r + ", 2;");
    emit_line("add.u32 " + off_r + ", " + off_r + ", " + r(21) + ";");
    emit_line("cvt.u64.u32 " + rd(19) + ", " + off_r + ";");
    emit_line("add.u64 " + rd(19) + ", " + scratch_rd + ", " + rd(19) + ";");
    emit_line("st.shared.u32 [" + rd(19) + "], " + src_r + ";");
  }

  void emit_spill_v_window_to_mma_scratch(const std::string &scratch_rd, int base_reg, uint8_t reg_count) {
    for (uint8_t reg = 0; reg < reg_count; ++reg) {
      emit_line("mov.u32 " + r(20) + ", " + std::to_string(reg) + ";");
      emit_line("mov.u32 " + r(22) + ", " + v(base_reg + static_cast<int>(reg)) + ";");
      emit_store_u32_to_mma_scratch(scratch_rd, r(20), r(0), r(22), r(23));
    }
    emit_warp_sync();
  }

  void emit_reload_v_window_from_mma_scratch(const std::string &scratch_rd, int base_reg, uint8_t reg_count) {
    for (uint8_t reg = 0; reg < reg_count; ++reg) {
      emit_line("mov.u32 " + r(20) + ", " + std::to_string(reg) + ";");
      emit_load_u32_from_mma_scratch(r(22), scratch_rd, r(20), r(0), r(23));
      emit_line("mov.u32 " + v(base_reg + static_cast<int>(reg)) + ", " + r(22) + ";");
    }
  }

  void emit_compute_tuple_logical_coord(const sbt::ptx::mma::ScalarTupleValue &value, const std::string &row_r, const std::string &col_r) {
    const std::string lane_r = (value.lane_xor_mask == 0u) ? r(0) : col_r;
    if (value.lane_xor_mask != 0u) emit_line("xor.b32 " + lane_r + ", " + r(0) + ", " + std::to_string(value.lane_xor_mask) + ";");
    emit_line("shr.u32 " + row_r + ", " + lane_r + ", " + std::to_string(value.lane_row_shift) + ";");
    if (value.tile_row_base != 0u) emit_line("add.u32 " + row_r + ", " + row_r + ", " + std::to_string(value.tile_row_base) + ";");
    emit_line("and.b32 " + col_r + ", " + lane_r + ", " + std::to_string(value.lane_col_mask) + ";");
    if (value.lane_col_shift != 0u) emit_line("shl.b32 " + col_r + ", " + col_r + ", " + std::to_string(value.lane_col_shift) + ";");
    const uint32_t col_bias = static_cast<uint32_t>(value.tile_col_base) + static_cast<uint32_t>(value.lane_col_bias);
    if (col_bias != 0u) emit_line("add.u32 " + col_r + ", " + col_r + ", " + std::to_string(col_bias) + ";");
  }

  void emit_compute_window_index_from_logical_coord(const sbt::DecodedInst &di, sbt::ptx::mma::OperandRole role, uint8_t slice_col_offset,
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

  void emit_compute_b_window_index_from_logical_coord(const sbt::DecodedInst &di, const sbt::ptx::mma::BSourceWindowPlan &plan,
                                                      const std::string &logical_n_r, const std::string &logical_k_r, const std::string &idx_r,
                                                      const std::string &tmp_r) {
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

  void emit_load_scalar_value_from_spilled_window(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi,
                                                  sbt::ptx::mma::OperandRole role, uint8_t slice_col_offset,
                                                  const sbt::ptx::mma::ScalarTupleValue &value,
                                                  const std::string &scratch_rd, const std::string &dst_r, const std::string &row_r,
                                                  const std::string &col_r, const std::string &idx_r, const std::string &reg_r,
                                                  const std::string &lane_r, const std::string &tmp_r, const std::string &tmp2_r) {
    emit_compute_tuple_logical_coord(value, row_r, col_r);
    emit_compute_window_index_from_logical_coord(di, role, slice_col_offset, row_r, col_r, idx_r, tmp_r);

    if (role == sbt::ptx::mma::OperandRole::A || role == sbt::ptx::mma::OperandRole::B) {
      if (di.mma.wide_ab) {
        emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 5;");
        emit_line("and.b32 " + lane_r + ", " + idx_r + ", 31;");
        emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
        return;
      }
      emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 6;");
      emit_line("shr.u32 " + lane_r + ", " + idx_r + ", 1;");
      emit_line("and.b32 " + lane_r + ", " + lane_r + ", 31;");
      emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
      emit_line("and.b32 " + tmp2_r + ", " + idx_r + ", 1;");
      emit_line("setp.ne.u32 " + p(6) + ", " + tmp2_r + ", 0;");
      emit_line("@" + p(6) + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
      emit_line("and.b32 " + dst_r + ", " + dst_r + ", 0xffff;");
      return;
    }

    emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 5;");
    emit_line("and.b32 " + lane_r + ", " + idx_r + ", 31;");
    emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
  }

  void emit_load_b_scalar_value_from_spilled_window(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi,
                                                    const sbt::ptx::mma::BSourceWindowPlan &plan,
                                                    const sbt::ptx::mma::ScalarTupleValue &value,
                                                    const std::string &scratch_rd, const std::string &dst_r, const std::string &row_r,
                                                    const std::string &col_r, const std::string &idx_r, const std::string &reg_r,
                                                    const std::string &lane_r, const std::string &tmp_r, const std::string &tmp2_r) {
    emit_compute_tuple_logical_coord(value, row_r, col_r);
    emit_compute_b_window_index_from_logical_coord(di, plan, row_r, col_r, idx_r, tmp_r);

    if (plan.source_pack == sbt::ptx::mma::PackMode::Wide32) {
      emit_line("shr.u32 " + reg_r + ", " + idx_r + ", " + std::to_string(plan.reg_shift) + ";");
      emit_line("and.b32 " + lane_r + ", " + idx_r + ", " + std::to_string(plan.lane_mask) + ";");
      emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
      return;
    }

    emit_line("shr.u32 " + reg_r + ", " + idx_r + ", " + std::to_string(plan.reg_shift) + ";");
    emit_line("shr.u32 " + lane_r + ", " + idx_r + ", " + std::to_string(plan.lane_shift) + ";");
    emit_line("and.b32 " + lane_r + ", " + lane_r + ", " + std::to_string(plan.lane_mask) + ";");
    emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
    emit_line("and.b32 " + tmp2_r + ", " + idx_r + ", 1;");
    if (plan.half_xor != 0u) emit_line("xor.b32 " + tmp2_r + ", " + tmp2_r + ", " + std::to_string(plan.half_xor) + ";");
    emit_line("setp.ne.u32 " + p(6) + ", " + tmp2_r + ", 0;");
    emit_line("@" + p(6) + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
    emit_line("and.b32 " + dst_r + ", " + dst_r + ", 0xffff;");
  }

  void emit_store_scalar_value_to_spilled_cd_window(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset,
                                                    const sbt::ptx::mma::ScalarTupleValue &value, const std::string &scratch_rd,
                                                    const std::string &src_r,
                                                    const std::string &row_r, const std::string &col_r, const std::string &idx_r,
                                                    const std::string &reg_r, const std::string &lane_r, const std::string &tmp_r) {
    emit_compute_tuple_logical_coord(value, row_r, col_r);
    emit_compute_window_index_from_logical_coord(di, sbt::ptx::mma::OperandRole::D, slice_col_offset, row_r, col_r, idx_r, tmp_r);
    emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 5;");
    emit_line("and.b32 " + lane_r + ", " + idx_r + ", 31;");
    emit_store_u32_to_mma_scratch(scratch_rd, reg_r, lane_r, src_r, tmp_r);
  }

  void emit_materialize_tuple_regs(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, sbt::ptx::mma::OperandRole role,
                                   uint8_t slice_col_offset, const std::string &scratch_rd, const std::vector<std::string> &dst_regs) {
    const auto pack = sbt::ptx::mma::tuple_pack(abi, role);
    const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, role);
    require(dst_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));
    require(role != sbt::ptx::mma::OperandRole::B, EmitError("invalid.mma.plan", func_name, di.pc, mma_detail(di.mma)));

    if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
      for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
        for (uint8_t elem = 0; elem < 2u; ++elem) {
          const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, role, tuple_reg, elem);
          const std::string dst_word = (elem == 0u) ? r(16) : r(17);
          emit_load_scalar_value_from_spilled_window(di, abi, role, slice_col_offset, value, scratch_rd, dst_word, r(14), r(15), r(22), r(23),
                                                     r(24), r(25), r(13));
          emit_line("and.b32 " + dst_word + ", " + dst_word + ", 0xffff;");
        }
        emit_line("shl.b32 " + r(17) + ", " + r(17) + ", 16;");
        emit_line("or.b32 " + dst_regs[tuple_reg] + ", " + r(16) + ", " + r(17) + ";");
      }
      return;
    }

    for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
      const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, role, tuple_reg, 0u);
      emit_load_scalar_value_from_spilled_window(di, abi, role, slice_col_offset, value, scratch_rd, r(16), r(14), r(15), r(22), r(23), r(24),
                                                 r(25), r(13));
      if (role == sbt::ptx::mma::OperandRole::C || role == sbt::ptx::mma::OperandRole::D) emit_line("mov.b32 " + dst_regs[tuple_reg] + ", " + r(16) + ";");
      else emit_line("mov.u32 " + dst_regs[tuple_reg] + ", " + r(16) + ";");
    }
  }

  void emit_materialize_b_tuple_regs(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, const sbt::ptx::mma::BSourceWindowPlan &plan,
                                     const std::string &scratch_rd, const std::vector<std::string> &dst_regs) {
    const auto pack = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::B);
    const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::B);
    require(dst_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));
    require(plan.native_window_n == abi.n, EmitError("invalid.mma.b_slice", func_name, di.pc, mma_detail(di.mma)));

    if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
      for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
        for (uint8_t elem = 0; elem < 2u; ++elem) {
          const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::B, tuple_reg, elem);
          const std::string dst_word = (elem == 0u) ? r(16) : r(17);
          emit_load_b_scalar_value_from_spilled_window(di, abi, plan, value, scratch_rd, dst_word, r(14), r(15), r(22), r(23), r(24), r(25),
                                                       r(13));
          emit_line("and.b32 " + dst_word + ", " + dst_word + ", 0xffff;");
        }
        emit_line("shl.b32 " + r(17) + ", " + r(17) + ", 16;");
        emit_line("or.b32 " + dst_regs[tuple_reg] + ", " + r(16) + ", " + r(17) + ";");
      }
      return;
    }

    for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
      const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::B, tuple_reg, 0u);
      emit_load_b_scalar_value_from_spilled_window(di, abi, plan, value, scratch_rd, r(16), r(14), r(15), r(22), r(23), r(24), r(25), r(13));
      emit_line("mov.u32 " + dst_regs[tuple_reg] + ", " + r(16) + ";");
    }
  }

  void emit_store_d_tuple_to_spilled_cd_window(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset,
                                               const std::string &scratch_rd, const std::vector<std::string> &src_regs) {
    const auto pack = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D);
    const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::D);
    require(src_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));

    if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
      for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
        emit_line("mov.b32 {" + h(0) + ", " + h(1) + "}, " + src_regs[tuple_reg] + ";");
        for (uint8_t elem = 0; elem < 2u; ++elem) {
          const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::D, tuple_reg, elem);
          emit_line("mov.b16 " + u16(2) + ", " + h(static_cast<int>(elem)) + ";");
          emit_line("cvt.u32.u16 " + r(16) + ", " + u16(2) + ";");
          emit_store_scalar_value_to_spilled_cd_window(di, abi, slice_col_offset, value, scratch_rd, r(16),
                                                       r(14), r(15), r(17), r(18), r(19), r(20));
        }
      }
      return;
    }

    for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
      const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::D, tuple_reg, 0u);
      emit_line("mov.b32 " + r(16) + ", " + src_regs[tuple_reg] + ";");
      emit_store_scalar_value_to_spilled_cd_window(di, abi, slice_col_offset, value, scratch_rd, r(16), r(14), r(15), r(17), r(18), r(19),
                                                   r(20));
    }
  }

  void emit_native_mma_sync(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset) {
    std::vector<std::string> a_tuple_regs;
    a_tuple_regs.reserve(kMmaATupleRegIds.size());
    for (const int reg_id : kMmaATupleRegIds) a_tuple_regs.push_back(r(reg_id));

    std::vector<std::string> b_tuple_regs;
    b_tuple_regs.reserve(kMmaBTupleRegIds.size());
    for (const int reg_id : kMmaBTupleRegIds) b_tuple_regs.push_back(r(reg_id));

    emit_compute_mma_scratch_base(rd(18), di.pc);

    emit_spill_v_window_to_mma_scratch(rd(18), di.mma.rs1_base, di.mma.a_regs_per_thread);
    emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::A, 0u, rd(18), a_tuple_regs);

    const auto b_plan = sbt::ptx::mma::b_source_window_plan(di.mma, abi, slice_col_offset);
    emit_spill_v_window_to_mma_scratch(rd(18), di.mma.rs2_base + static_cast<int>(b_plan.reg_offset), b_plan.reg_count);
    emit_materialize_b_tuple_regs(di, abi, b_plan, rd(18), b_tuple_regs);

    emit_spill_v_window_to_mma_scratch(rd(18), di.mma.rd_base, di.mma.c_regs_per_thread);
    const bool packed_d = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D) == sbt::ptx::mma::PackMode::Packed16x2;
    if (packed_d) {
      emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::C, slice_col_offset, rd(18), {r(22), r(23)});
      emit_line(std::string(abi.ptx_opcode) + " {" + r(24) + ", " + r(25) + "}, {" + a_tuple_regs[0] + ", " + a_tuple_regs[1] + ", " +
                a_tuple_regs[2] + ", " + a_tuple_regs[3] + "}, {" + b_tuple_regs[0] + ", " + b_tuple_regs[1] + "}, {" + r(22) + ", " + r(23) +
                "};");
      emit_store_d_tuple_to_spilled_cd_window(di, abi, slice_col_offset, rd(18), {r(24), r(25)});
    } else {
      emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::C, slice_col_offset, rd(18), {f(0), f(1), f(2), f(3)});
      emit_line(std::string(abi.ptx_opcode) + " {" + f(4) + ", " + f(5) + ", " + f(6) + ", " + f(7) + "}, {" + a_tuple_regs[0] + ", " +
                a_tuple_regs[1] + ", " + a_tuple_regs[2] + ", " + a_tuple_regs[3] + "}, {" + b_tuple_regs[0] + ", " + b_tuple_regs[1] + "}, {" +
                f(0) + ", " + f(1) + ", " + f(2) + ", " + f(3) + "};");
      emit_store_d_tuple_to_spilled_cd_window(di, abi, slice_col_offset, rd(18), {f(4), f(5), f(6), f(7)});
    }

    emit_warp_sync();
    emit_reload_v_window_from_mma_scratch(rd(18), di.mma.rd_base, di.mma.c_regs_per_thread);
  }

  void emit_mma_inst(const sbt::DecodedInst &di) {
    require(di.mma.valid, EmitError("invalid.mma.metadata", func_name, di.pc, di.name));
    const std::string detail = mma_detail(di.mma);

    if (di.mma.cd_type == sbt::MmaCdType::Fp16) {
      throw EmitError("unsupported.mma.fp16_fp16_contract_pending", func_name, di.pc,
                      detail + " note=Ventus LLVM/Spike MMA fp16->fp16 ABI/layout contract is not confirmed yet; keep this path fail-fast");
    }

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
      emit_native_mma_sync(di, *abi, 0u);
      return;
    case sbt::MmaLoweringClass::CompositeLowering:
      emit_native_mma_sync(di, *abi, 0u);
      emit_native_mma_sync(di, *abi, 8u);
      return;
    case sbt::MmaLoweringClass::NativeWmma:
      throw EmitError("unsupported.mma.native_wmma", func_name, di.pc, detail);
    case sbt::MmaLoweringClass::Unsupported:
      throw EmitError("unsupported.mma.lowering", func_name, di.pc, detail);
    }
    throw EmitError("unsupported.mma.lowering", func_name, di.pc, detail);
  }

  void emit_one_inst(const sbt::cfg::BundleInst &bi) {
    const sbt::DecodedInst &di = bi.inst;
    const uint32_t pc = di.pc;
    const uint32_t bundle_pc = bi.pc;
    const uint32_t inst_pc = bi.inst_pc;

    if (opt.include_comments) {
      emit_line("// " + hex_u32(bi.pc) + " " + di.name);
    }

    // No-ops under structured translation.
    if (di.name == "setrpc" || di.name == "join" || di.name == "vsetvli") {
      return;
    }

    if (di.name == "endprg" || is_ret(di)) {
      if (is_entry) {
        emit_entry_pds_pool_release(pc);
      }
      if (!is_entry) emit_store_mutable_state_blob("__sbt_mutable_state_out");
      emit_line("ret;");
      return;
    }

    if (di.name == "barrier") {
      emit_line("bar.sync 0;");
      return;
    }

    // Calls: support a small inlined builtin set, plus direct calls to emitted `.func`s.
    if (is_call(di)) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
      auto it = sym_by_addr.find(target);
      require(it != sym_by_addr.end(), EmitError("unsupported.call", func_name, pc, "target=" + hex_u32(target)));
      const std::string &callee = it->second;

      emit_line("mov.u32 " + r(14) + ", " + hex_u32(inst_pc + 4) + ";");
      emit_st_x_u32_scalar(di.rd, r(14), pc);

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
      emit_direct_call(jt->second);
      return;
    }

    // Scalar jumps (unconditional).
    if (is_uncond_jump(di)) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
      const uint32_t src_block = block_start_of_pc(bundle_pc);
      const uint32_t dst_block = block_start_of_pc(target);
      emit_line("bra " + target_label_for_edge(src_block, dst_block) + ";");
      return;
    }

    // Scalar conditional branches.
    if (is_scalar_branch(di.name) && di.imm_kind == sbt::ImmKind::B13) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
      const uint32_t fallthrough = inst_pc + 4;
      const uint32_t src_block = block_start_of_pc(bundle_pc);
      const uint32_t dst_t = block_start_of_pc(target);
      const uint32_t dst_f = block_start_of_pc(fallthrough);

      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_ld_x_u32_all(r(15), di.rs2, pc);

      if (di.name == "beq") emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bne") emit_line("setp.ne.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "blt") emit_line("setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bge") emit_line("setp.ge.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bltu") emit_line("setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else if (di.name == "bgeu") emit_line("setp.ge.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      else throw EmitError("unsupported.inst", func_name, pc, di.name);

      emit_line("@" + p(1) + " bra.uni " + target_label_for_edge(src_block, dst_t) + ";");
      emit_line("bra.uni " + target_label_for_edge(src_block, dst_f) + ";");
      return;
    }

    // Vector conditional branches.
    if (is_vector_branch(di.name) && di.imm_kind == sbt::ImmKind::B13) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
      const uint32_t fallthrough = inst_pc + 4;
      const uint32_t src_block = block_start_of_pc(bundle_pc);
      const uint32_t dst_t = block_start_of_pc(target);
      const uint32_t dst_f = block_start_of_pc(fallthrough);

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

      emit_line("@" + p(1) + " bra " + target_label_for_edge(src_block, dst_t) + ";");
      emit_line("bra " + target_label_for_edge(src_block, dst_f) + ";");
      return;
    }

    // CSR ops (prototype: treat Ventus CSRs as read-only for bring-up).
    if (di.name == "csrrw" || di.name == "csrrs" || di.name == "csrrc" || di.name == "csrrwi" || di.name == "csrrsi" || di.name == "csrrci") {
      require(scalar_exec_kind_for_inst(di) == ScalarExecKind::UniformPure,
              EmitError("invalid.scalar_exec", func_name, pc, std::string(di.name)));
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
        emit_compute_csr_pds_u32(r(14), /*scalar=*/true);
      } else if (csr == 0x808u) { // CSR_GDX
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.x;");
      } else if (csr == 0x809u) { // CSR_GDY
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.y;");
      } else if (csr == 0x80au) { // CSR_GDZ
        emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", %ctaid.z;");
      } else if (csr == 0x80bu) { // CSR_PRINT
        emit_load_knl_u32_scalar(r(14), kKnlPrintAddrOffset, pc);
      } else {
        throw EmitError("unsupported.csr", func_name, pc, "csr=" + hex_u32(csr));
      }

      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }

    // Scalar loads/stores (uniform ops).
    if (is_scalar_load(di.name) && di.imm_kind == sbt::ImmKind::I12) {
      require(scalar_exec_kind_for_inst(di) == ScalarExecKind::UniformPure,
              EmitError("invalid.scalar_exec", func_name, pc, std::string(di.name)));
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
      require(scalar_exec_kind_for_inst(di) == ScalarExecKind::ExternallySideEffecting,
              EmitError("invalid.scalar_exec", func_name, pc, std::string(di.name)));
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
      emit_compute_csr_pds_u32(r(24), /*scalar=*/false);

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
      emit_compute_csr_pds_u32(r(24), /*scalar=*/false);

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
      require(scalar_exec_kind_for_inst(di) == ScalarExecKind::FixedLaneSensitive,
              EmitError("invalid.scalar_exec", func_name, pc, std::string(di.name)));
      emit_trap_if_lane_inactive(/*lane=*/0u);
      emit_line("setp.eq.u32 " + p(2) + ", " + r(0) + ", 0;");
      emit_line("@" + p(2) + " mov.u32 " + x(di.rd) + ", " + v(di.rs2) + ";");
      emit_read_activemask(r(1));
      emit_line("shfl.sync.idx.b32 " + x(di.rd) + ", " + x(di.rd) + ", 0, 0x1f, " + r(1) + ";");
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

    if (di.mma.valid || (di.custom.valid && di.custom.family == sbt::CustomFamily::Mma)) {
      emit_mma_inst(di);
      return;
    }

    auto unpack_u32_to_halves = [&](const std::string &src_u32, int lo_h16, int hi_h16) {
      emit_line("mov.b32 {" + h(lo_h16) + ", " + h(hi_h16) + "}, " + src_u32 + ";");
    };
    auto pack_halves_to_u32 = [&](const std::string &dst_u32, int lo_h16, int hi_h16) {
      emit_line("mov.b32 " + dst_u32 + ", {" + h(lo_h16) + ", " + h(hi_h16) + "};");
    };
    auto emit_tanh_f32 = [&](const std::string &dst_f, const std::string &src_f) {
      emit_line("mul.rn.f32 " + f(6) + ", " + src_f + ", -2.8853900817779268;");
      emit_line("ex2.approx.f32 " + f(6) + ", " + f(6) + ";");
      emit_line("add.rn.f32 " + f(7) + ", " + f(6) + ", 1.0;");
      emit_line("rcp.approx.f32 " + f(7) + ", " + f(7) + ";");
      emit_line("mul.rn.f32 " + f(7) + ", " + f(7) + ", 2.0;");
      emit_line("add.rn.f32 " + dst_f + ", " + f(7) + ", -1.0;");
    };
    auto emit_silu_f32 = [&](const std::string &dst_f, const std::string &src_f) {
      emit_line("mul.rn.f32 " + f(6) + ", " + src_f + ", -1.4426950408889634;");
      emit_line("ex2.approx.f32 " + f(6) + ", " + f(6) + ";");
      emit_line("add.rn.f32 " + f(7) + ", " + f(6) + ", 1.0;");
      emit_line("rcp.approx.f32 " + f(7) + ", " + f(7) + ";");
      emit_line("mul.rn.f32 " + dst_f + ", " + src_f + ", " + f(7) + ";");
    };
    auto emit_gelu_f32 = [&](const std::string &dst_f, const std::string &src_f) {
      emit_line("mul.rn.f32 " + f(6) + ", " + src_f + ", " + src_f + ";");
      emit_line("mul.rn.f32 " + f(6) + ", " + f(6) + ", " + src_f + ";");
      emit_line("mad.rn.f32 " + f(6) + ", " + f(6) + ", 0.044715, " + src_f + ";");
      emit_line("mul.rn.f32 " + f(6) + ", " + f(6) + ", 0.7978845608028654;");
      emit_tanh_f32(f(7), f(6));
      emit_line("add.rn.f32 " + f(7) + ", " + f(7) + ", 1.0;");
      emit_line("mul.rn.f32 " + f(7) + ", " + f(7) + ", 0.5;");
      emit_line("mul.rn.f32 " + dst_f + ", " + src_f + ", " + f(7) + ";");
    };
    auto emit_custom_sfu_f32 = [&](const std::string &dst_f, const std::string &src_f, sbt::CustomSubOp subop) {
      switch (subop) {
      case sbt::CustomSubOp::Ex2: emit_line("ex2.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Lg2: emit_line("lg2.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Rcp: emit_line("rcp.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Sqrt: emit_line("sqrt.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Rsqrt: emit_line("rsqrt.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Sin: emit_line("sin.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Cos: emit_line("cos.approx.f32 " + dst_f + ", " + src_f + ";"); return;
      case sbt::CustomSubOp::Tanh: emit_tanh_f32(dst_f, src_f); return;
      case sbt::CustomSubOp::Gelu: emit_gelu_f32(dst_f, src_f); return;
      case sbt::CustomSubOp::Silu: emit_silu_f32(dst_f, src_f); return;
      default: throw EmitError("unsupported.custom.sfu", func_name, pc, di.name);
      }
    };
    auto emit_custom_rsqrt_dual_lane = [&](bool bf16_kind, int src_h16, const std::string &src_f32,
                                           const std::string &dst_f32, int dst_h16) {
      const std::string exp_mask = bf16_kind ? "0x7f80" : "0x7c00";
      const std::string frac_mask = bf16_kind ? "0x007f" : "0x03ff";
      const std::string pos_inf = bf16_kind ? "0x7f80" : "0x7c00";
      const std::string convert_to_f32 = bf16_kind ? "cvt.f32.bf16 " : "cvt.f32.f16 ";
      const std::string convert_from_f32 = bf16_kind ? "cvt.rn.bf16.f32 " : "cvt.rn.f16.f32 ";
      emit_line("mov.b16 " + u16(0) + ", " + h(src_h16) + ";");
      emit_line("cvt.u32.u16 " + r(20) + ", " + u16(0) + ";");
      emit_line("and.b32 " + r(21) + ", " + r(20) + ", 0x8000;");
      emit_line("and.b32 " + r(22) + ", " + r(20) + ", " + exp_mask + ";");
      emit_line("and.b32 " + r(23) + ", " + r(20) + ", " + frac_mask + ";");
      emit_line("setp.ne.u32 " + p(2) + ", " + r(21) + ", 0;");
      emit_line("setp.eq.u32 " + p(3) + ", " + r(22) + ", 0;");
      emit_line("setp.eq.u32 " + p(4) + ", " + r(22) + ", " + exp_mask + ";");
      emit_line("setp.eq.u32 " + p(5) + ", " + r(23) + ", 0;");
      emit_line("and.pred " + p(6) + ", " + p(3) + ", " + p(5) + ";");
      emit_line("and.pred " + p(7) + ", " + p(4) + ", " + p(5) + ";");
      emit_line("not.pred " + p(8) + ", " + p(5) + ";");
      emit_line("and.pred " + p(8) + ", " + p(4) + ", " + p(8) + ";");
      emit_line("not.pred " + p(9) + ", " + p(6) + ";");
      emit_line("and.pred " + p(9) + ", " + p(2) + ", " + p(9) + ";");
      emit_line("not.pred " + p(10) + ", " + p(2) + ";");
      emit_line("and.pred " + p(10) + ", " + p(7) + ", " + p(10) + ";");
      emit_line(convert_to_f32 + src_f32 + ", " + h(src_h16) + ";");
      emit_custom_sfu_f32(dst_f32, src_f32, sbt::CustomSubOp::Rsqrt);
      emit_line(convert_from_f32 + h(dst_h16) + ", " + dst_f32 + ";");
      emit_line("@" + p(6) + " mov.b16 " + h(dst_h16) + ", " + pos_inf + ";");
      emit_line("@" + p(9) + " mov.b16 " + h(dst_h16) + ", 0x7fff;");
      emit_line("@" + p(10) + " mov.b16 " + h(dst_h16) + ", 0;");
      emit_line("@" + p(8) + " mov.b16 " + h(dst_h16) + ", 0x7fff;");
    };
    auto subop_from_sfu_name = [&]() -> sbt::CustomSubOp {
      if (di.name.rfind("vex2_approx_", 0) == 0) return sbt::CustomSubOp::Ex2;
      if (di.name.rfind("vlg2_approx_", 0) == 0) return sbt::CustomSubOp::Lg2;
      if (di.name.rfind("vrcp_approx_", 0) == 0) return sbt::CustomSubOp::Rcp;
      if (di.name.rfind("vsqrt_approx_", 0) == 0) return sbt::CustomSubOp::Sqrt;
      if (di.name.rfind("vrsqrt_approx_", 0) == 0) return sbt::CustomSubOp::Rsqrt;
      if (di.name.rfind("vsin_approx_", 0) == 0) return sbt::CustomSubOp::Sin;
      if (di.name.rfind("vcos_approx_", 0) == 0) return sbt::CustomSubOp::Cos;
      if (di.name.rfind("vtanh_approx_", 0) == 0) return sbt::CustomSubOp::Tanh;
      if (di.name.rfind("vgelu_approx_", 0) == 0) return sbt::CustomSubOp::Gelu;
      if (di.name.rfind("vsilu_approx_", 0) == 0) return sbt::CustomSubOp::Silu;
      return sbt::CustomSubOp::None;
    };

    if (di.name == "shuffle_idx" || di.name == "shuffle_up" || di.name == "shuffle_down" || di.name == "shuffle_bfly") {
      emit_read_activemask(r(1));
      emit_line("mov.u32 " + r(14) + ", " + std::to_string(di.imm & 31) + ";");
      if (di.name == "shuffle_idx") emit_line("shfl.sync.idx.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ", 0x1f, " + r(1) + ";");
      else if (di.name == "shuffle_up") emit_line("shfl.sync.up.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ", 0x0, " + r(1) + ";");
      else if (di.name == "shuffle_down") emit_line("shfl.sync.down.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ", 0x1f, " + r(1) + ";");
      else emit_line("shfl.sync.bfly.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ", 0x1f, " + r(1) + ";");
      return;
    }

    if (di.name == "vcvt_f32_fp16") {
      unpack_u32_to_halves(v(di.rs2), 0, 1);
      emit_line("cvt.f32.f16 " + f(0) + ", " + h(0) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
      return;
    }
    if (di.name == "vcvt_f16_fp32") {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rn.f16.f32 " + h(0) + ", " + f(0) + ";");
      emit_line("mov.b16 " + h(1) + ", 0;");
      pack_halves_to_u32(r(14), 0, 1);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.name == "vcvt_fp32_bf16") {
      unpack_u32_to_halves(v(di.rs2), 0, 1);
      emit_line("cvt.f32.bf16 " + f(0) + ", " + h(0) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
      return;
    }
    if (di.name == "vcvt_bf16_fp32") {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rn.bf16.f32 " + h(0) + ", " + f(0) + ";");
      emit_line("mov.b16 " + h(1) + ", 0;");
      pack_halves_to_u32(r(14), 0, 1);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }

    if (di.name == "vadd_f16x2" || di.name == "vmul_f16x2" || di.name == "vfma_f16x2") {
      if (di.name == "vadd_f16x2") emit_line("add.rn.f16x2 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      else if (di.name == "vmul_f16x2") emit_line("mul.rn.f16x2 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      else emit_line("fma.rn.f16x2 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }

    if (di.name == "vadd_bf16x2" || di.name == "vmul_bf16x2" || di.name == "vfma_bf16x2") {
      if (di.name == "vfma_bf16x2") {
        emit_line("fma.rn.bf16x2 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
        emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
        return;
      }
      unpack_u32_to_halves(v(di.rs1), 0, 1);
      unpack_u32_to_halves(v(di.rs2), 2, 3);
      emit_line("cvt.f32.bf16 " + f(0) + ", " + h(0) + ";");
      emit_line("cvt.f32.bf16 " + f(1) + ", " + h(1) + ";");
      emit_line("cvt.f32.bf16 " + f(2) + ", " + h(2) + ";");
      emit_line("cvt.f32.bf16 " + f(3) + ", " + h(3) + ";");
      if (di.name == "vadd_bf16x2") {
        emit_line("add.rn.f32 " + f(4) + ", " + f(0) + ", " + f(2) + ";");
        emit_line("add.rn.f32 " + f(5) + ", " + f(1) + ", " + f(3) + ";");
      } else {
        emit_line("mul.rn.f32 " + f(4) + ", " + f(0) + ", " + f(2) + ";");
        emit_line("mul.rn.f32 " + f(5) + ", " + f(1) + ", " + f(3) + ";");
      }
      emit_line("cvt.rn.bf16.f32 " + h(4) + ", " + f(4) + ";");
      emit_line("cvt.rn.bf16.f32 " + h(5) + ", " + f(5) + ";");
      pack_halves_to_u32(r(14), 4, 5);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }

    if (di.name.find("_approx_f32") != std::string::npos && di.name.rfind("v", 0) == 0) {
      const sbt::CustomSubOp subop = (di.custom.valid && di.custom.subop != sbt::CustomSubOp::None) ? di.custom.subop : subop_from_sfu_name();
      require(subop != sbt::CustomSubOp::None, EmitError("unsupported.custom.sfu", func_name, pc, di.name));
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_custom_sfu_f32(f(1), f(0), subop);
      emit_line("mov.b32 " + v(di.rd) + ", " + f(1) + ";");
      return;
    }

    if (di.name.find("_approx_f16x2") != std::string::npos) {
      const sbt::CustomSubOp subop = (di.custom.valid && di.custom.subop != sbt::CustomSubOp::None) ? di.custom.subop : subop_from_sfu_name();
      require(subop != sbt::CustomSubOp::None, EmitError("unsupported.custom.sfu", func_name, pc, di.name));
      unpack_u32_to_halves(v(di.rs2), 0, 1);
      if (subop == sbt::CustomSubOp::Rsqrt) {
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/false, 0, f(0), f(2), 2);
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/false, 1, f(1), f(3), 3);
      } else {
        emit_line("cvt.f32.f16 " + f(0) + ", " + h(0) + ";");
        emit_line("cvt.f32.f16 " + f(1) + ", " + h(1) + ";");
        emit_custom_sfu_f32(f(2), f(0), subop);
        emit_custom_sfu_f32(f(3), f(1), subop);
        emit_line("cvt.rn.f16.f32 " + h(2) + ", " + f(2) + ";");
        emit_line("cvt.rn.f16.f32 " + h(3) + ", " + f(3) + ";");
      }
      pack_halves_to_u32(r(14), 2, 3);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }

    if (di.name.find("_approx_bf16x2") != std::string::npos) {
      const sbt::CustomSubOp subop = (di.custom.valid && di.custom.subop != sbt::CustomSubOp::None) ? di.custom.subop : subop_from_sfu_name();
      require(subop != sbt::CustomSubOp::None, EmitError("unsupported.custom.sfu", func_name, pc, di.name));
      unpack_u32_to_halves(v(di.rs2), 0, 1);
      if (subop == sbt::CustomSubOp::Rsqrt) {
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/true, 0, f(0), f(2), 2);
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/true, 1, f(1), f(3), 3);
      } else {
        emit_line("cvt.f32.bf16 " + f(0) + ", " + h(0) + ";");
        emit_line("cvt.f32.bf16 " + f(1) + ", " + h(1) + ";");
        emit_custom_sfu_f32(f(2), f(0), subop);
        emit_custom_sfu_f32(f(3), f(1), subop);
        emit_line("cvt.rn.bf16.f32 " + h(2) + ", " + f(2) + ";");
        emit_line("cvt.rn.bf16.f32 " + h(3) + ", " + f(3) + ";");
      }
      pack_halves_to_u32(r(14), 2, 3);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
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
    //  - global_base: backing buffer for [global_base_vaddr, 0x1_0000_0000)
    //  - knl_vaddr: Ventus numeric address of metadata buffer (u32)
    //  - pds_base_vaddr: Ventus numeric address of the global PDS buffer base (u32)
    //  - pds_size_per_thread: bytes of private memory per thread (u32)
    //  - pds_bitmap_base_vaddr: Ventus numeric address of PDS allocation bitmap (u32)
    //  - pds_pool_num_blocks: number of reusable WG blocks in PDS pool (u32)
    emit_raw(".visible .entry " + ptx_name + "(\n");
    emit_raw("    .param .u64 global_base,\n");
    emit_raw("    .param .u32 knl_vaddr,\n");
    emit_raw("    .param .u32 pds_base_vaddr,\n");
    emit_raw("    .param .u32 pds_size_per_thread,\n");
    emit_raw("    .param .u32 pds_bitmap_base_vaddr,\n");
    emit_raw("    .param .u32 pds_pool_num_blocks\n");
    emit_raw(")\n{\n");

    emit_line(".reg .b32 %r<32>;");
    emit_line(".reg .b64 %rd<32>;");
    emit_line(".reg .pred %p<16>;");
    emit_line(".reg .f32 %f<16>;");
    emit_line(".reg .b16 %h<16>;");
    emit_line(".reg .u8 %ub<4>;");
    emit_line(".reg .u16 %uh<16>;");
    emit_line(".reg .b32 %x<256>;");
    emit_line(".reg .b32 %v<256>;");

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

    // x2 = shared_base + warp_id * ldsStackSizePerWf
    emit_line("mul.lo.u32 " + r(15) + ", " + r(10) + ", " + r(17) + ";");
    emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_st_x_u32_scalar(/*x2=*/2, r(15), /*pc_for_err=*/cfg.start);

    // Match `_start` ABI: s0 (x8) points to the base of the kernel LDS region:
    //   s0 = CSR_LDS + CSR_NUMW*ldsStackSizePerWf
    // In this backend `shared_base_vaddr` models the CSR_LDS numeric base, and `warps_per_block` models CSR_NUMW.
    // Note: kernels may further adjust s0 in their own prologue (e.g. `addi s0, s0, <frame_bytes>`). We treat that
    // as frame allocation and do not attempt to compensate it here.
    emit_line("mul.lo.u32 " + r(15) + ", " + r(12) + ", " + r(17) + ";");
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

  void emit_func_prologue() {
    // Pass-through params computed in the caller and required for address mapping / CSR reads.
    emit_helper_func_signature(out, ptx_name);
    emit_raw("\n{\n");

    emit_line(".reg .b32 %r<32>;");
    emit_line(".reg .b64 %rd<32>;");
    emit_line(".reg .pred %p<16>;");
    emit_line(".reg .f32 %f<16>;");
    emit_line(".reg .b16 %h<16>;");
    emit_line(".reg .u8 %ub<4>;");
    emit_line(".reg .u16 %uh<16>;");
    emit_line(".reg .b32 %x<256>;");
    emit_line(".reg .b32 %v<256>;");

    emit_line("mov.u32 " + r(0) + ", %laneid;");
    emit_load_runtime_env_blob("__sbt_runtime_env_in");
    emit_load_machine_ctx_blob("__sbt_machine_ctx_in");
    emit_line("mov.u64 " + rd(2) + ", __sbt_shmem;");
    emit_line("mov.u64 " + rd(3) + ", 0;");
    emit_restore_mutable_state_blob("__sbt_mutable_state_in");
  }

  void emit_fallthrough_edge_if_needed(const sbt::cfg::BasicBlock &bb) {
    if (bb.inst_indices.empty()) return;
    std::optional<uint32_t> dst;
    for (const auto &edge : bb.succs) {
      if (edge.kind == sbt::cfg::EdgeKind::Fallthrough) {
        dst = edge.dst;
        break;
      }
    }
    if (!dst) return;

    const auto &last = cfg.insts[bb.inst_indices.back()].inst;
    if (is_uncond_jump(last)) return;
    if (is_ret(last) || last.name == "endprg") return;
    if (is_scalar_branch(last.name) || is_vector_branch(last.name)) return;
    emit_line("bra " + target_label_for_edge(bb.start, *dst) + ";");
  }

  void emit_body() {
    prepare_control_protocol();
    if (is_entry) emit_prologue();
    else emit_func_prologue();

    // Emit blocks in address order.
    for (const auto &bb : cfg.blocks) {
      emit_boundary_labels_for_block(bb.start);
      out << label_bb(bb.start) << ":\n";
      for (size_t idx : bb.inst_indices) {
        emit_one_inst(cfg.insts[idx]);
      }
      emit_fallthrough_edge_if_needed(bb);
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
  out << ".version 7.8\n";
  out << ".target sm_" << opt.sm << "\n";
  out << ".address_size 64\n\n";
  out << ".extern .shared .align 16 .b8 __sbt_shmem[];\n";
  out << ".shared .align 4 .u32 __sbt_pds_block_idx;\n";
  out << ".shared .align 4 .u32 __sbt_pds_wg_base;\n";
  out << ".shared .align 4 .u32 __sbt_pds_exit_count;\n\n";

  ModuleInfo mod;
  mod.ptx_name_by_addr = &ptx_name_by_addr;

  // Declare helper call prototypes first so ptxas can resolve forward calls.
  for (const auto &f : funcs) {
    emit_helper_func_signature(out, f.ptx_name);
    out << ";\n\n";
  }

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
