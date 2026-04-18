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

static bool is_uncond_jump(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::DirectJump && di.rd_class == sbt::RegClass::X &&
         di.rd == 0 && di.imm_kind == sbt::ImmKind::J21;
}

static bool is_call(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::DirectCall && di.rd_class == sbt::RegClass::X &&
         di.rd != 0 && di.imm_kind == sbt::ImmKind::J21;
}

static bool is_ret(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::Return && di.rd_class == sbt::RegClass::X &&
         di.rs1_class == sbt::RegClass::X && di.rd == 0 && di.rs1 == 1 && di.imm_kind == sbt::ImmKind::I12 && di.imm == 0;
}

static void require(bool ok, const EmitError &err) {
  if (!ok) throw err;
}

static ScalarExecKind scalar_exec_kind_for_inst(const sbt::DecodedInst &di, std::string_view func_name) {
  if (di.scalar_exec_kind != ScalarExecKind::None) return di.scalar_exec_kind;
  throw EmitError("missing.scalar_exec_metadata", std::string(func_name), di.pc, di.name);
}

static bool is_scalar_branch(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::ScalarBranch;
}

static bool is_vector_branch(const sbt::DecodedInst &di) {
  return di.emit.domain == EmitDomain::Control && di.emit.control_kind == ControlKind::VectorBranch;
}

static bool is_scalar_memory(const sbt::DecodedInst &di, MemAccessKind access) {
  return di.emit.domain == EmitDomain::ScalarMemory && di.emit.mem_access_kind == access;
}

static bool is_vector_memory(const sbt::DecodedInst &di, MemAccessKind access, MemoryAddrKind addr_kind) {
  return di.emit.domain == EmitDomain::VectorMemory && di.emit.mem_access_kind == access && di.emit.memory_addr_kind == addr_kind;
}

static bool is_scalar_int(const sbt::DecodedInst &di) { return di.emit.domain == EmitDomain::ScalarInteger; }

static bool is_scalar_fp(const sbt::DecodedInst &di) { return di.emit.domain == EmitDomain::ScalarFp; }

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
  std::ostringstream body;
  int tmp_label_id = 0;
  bool emitted_fp_dyn_note = false;
  bool control_protocol_ready = false;
  std::array<uint32_t, static_cast<size_t>(VirtualTempKind::Count)> virtual_temp_counts{};

  EmitCtx(const sbt::cfg::FunctionCfg &cfg_, const std::unordered_map<uint32_t, std::string> &sym_by_addr_,
          const std::string &func_name_, const std::string &ptx_name_, const Options &opt_, const ModuleInfo &mod_, bool is_entry_)
      : cfg(cfg_), sym_by_addr(sym_by_addr_), func_name(func_name_), ptx_name(ptx_name_), opt(opt_), mod(mod_), is_entry(is_entry_) {}

  void require_scalar_exec_kind(const sbt::DecodedInst &di, ScalarExecKind expected, uint32_t pc_for_err) const {
    require(scalar_exec_kind_for_inst(di, func_name) == expected,
            EmitError("invalid.scalar_exec", func_name, pc_for_err, std::string(di.name)));
  }

  void require_uniform_pure_scalar(const sbt::DecodedInst &di, uint32_t pc_for_err) const {
    require_scalar_exec_kind(di, ScalarExecKind::UniformPure, pc_for_err);
  }

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
  // %rd4: numeric-shared base (shared)  [shared_base_vaddr ..)  (stack + LDS)
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

  void emit_fixed_reg_decls() {
    out << "  .reg .b32 %r<32>;\n";
    out << "  .reg .b64 %rd<32>;\n";
    out << "  .reg .pred %p<16>;\n";
    out << "  .reg .f32 %f<16>;\n";
    out << "  .reg .b16 %h<16>;\n";
    out << "  .reg .u8 %ub<4>;\n";
    out << "  .reg .u16 %uh<16>;\n";
    out << "  .reg .b32 %x<256>;\n";
    out << "  .reg .b32 %v<256>;\n";
  }

  void emit_virtual_temp_reg_decls() {
    for (size_t kind = 0; kind < kVirtualTempSpecs.size(); ++kind) {
      const auto &spec = kVirtualTempSpecs[kind];
      for (uint32_t idx = 0; idx < virtual_temp_counts[kind]; ++idx) {
        out << "  .reg " << spec.ptx_type << " " << spec.name_prefix << idx << ";\n";
      }
    }
  }

  void emit_entry_header() {
    out << ".visible .entry " << ptx_name << "(\n";
    out << "    .param .u64 global_base,\n";
    out << "    .param .u32 knl_vaddr,\n";
    out << "    .param .u32 pds_base_vaddr,\n";
    out << "    .param .u32 pds_size_per_thread,\n";
    out << "    .param .u32 pds_bitmap_base_vaddr,\n";
    out << "    .param .u32 pds_pool_num_blocks\n";
    out << ")\n{\n";
  }

  void emit_func_header() {
    emit_helper_func_signature(out, ptx_name);
    out << "\n{\n";
  }

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
    const std::string active_mask = tmp_b32();
    const std::string lane_mask = tmp_b32();
    const std::string inactive = tmp_pred();
    require(lane < 32u, EmitError("unsupported.fixed_lane", func_name, cfg.start, "lane>=32"));
    emit_read_activemask(active_mask);
    emit_line("and.b32 " + lane_mask + ", " + active_mask + ", " + std::to_string(1u << lane) + ";");
    emit_line("setp.eq.u32 " + inactive + ", " + lane_mask + ", 0;");
    emit_line("@" + inactive + " trap;");
  }

  void emit_compute_csr_pds_u32(const std::string &dst_r, bool scalar) {
    const std::string pre = scalar ? scalar_prefix() : "";
    const std::string wg_base = tmp_b32();
    const std::string bytes_per_wave = tmp_b32();
    const std::string wave_offset = tmp_b32();
    emit_line(pre + "ld.shared.u32 " + wg_base + ", [__sbt_pds_wg_base];");
    emit_line(pre + "shl.b32 " + bytes_per_wave + ", " + r(29) + ", 5;");
    emit_line(pre + "mul.lo.u32 " + wave_offset + ", " + r(10) + ", " + bytes_per_wave + ";");
    emit_line(pre + "add.u32 " + dst_r + ", " + wg_base + ", " + wave_offset + ";");
  }

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
    emit_line("and.b32 " + r(17) + ", " + r(18) + ", " + r(19) + ";");
    emit_line("setp.eq.u32 " + p(7) + ", " + r(17) + ", 0;");
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
    emit_line("shl.b32 " + r(17) + ", " + r(29) + ", 5;"); // bytes per wf
    emit_line("mul.lo.u32 " + r(16) + ", " + r(12) + ", " + r(17) + ";"); // bytes per wg
    emit_line("cvt.u64.u32 " + rd(16) + ", " + r(18) + ";");
    emit_line("cvt.u64.u32 " + rd(17) + ", " + r(16) + ";");
    emit_line("mul.lo.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
    emit_line("cvt.u64.u32 " + rd(17) + ", " + r(28) + ";");
    emit_line("add.u64 " + rd(16) + ", " + rd(16) + ", " + rd(17) + ";");
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
        if (inst.emit.domain == EmitDomain::StructuredControl && inst.emit.structured_control_kind == StructuredControlKind::Join &&
            idx != bb.inst_indices.front()) {
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
    const std::string zero = tmp_b32();
    // Call/ret boundaries must export a warp-consistent leader metadata value, not a stale path-local leader.
    emit_select_leader_from_active_mask();
    emit_store_param_u32(blob_name, kMutableLeaderOffset, r(2));
    emit_line("mov.u32 " + zero + ", 0;");
    emit_store_param_u32(blob_name, kMutableXOffset, zero);
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

  std::string emit_ld_x_u32_all_tmp(int xreg, uint32_t pc_for_err) {
    const std::string dst = tmp_b32();
    emit_ld_x_u32_all(dst, xreg, pc_for_err);
    return dst;
  }

  std::string emit_ld_x_u32_scalar_tmp(int xreg, uint32_t pc_for_err) {
    const std::string dst = tmp_b32();
    emit_ld_x_u32_scalar(dst, xreg, pc_for_err);
    return dst;
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

  AddrMapTemps emit_prepare_addr_mapping(const std::string &addr_r, uint32_t pc_for_err) {
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
    emit_line("add.u64 " + temps.shared_ptr + ", " + rd(4) + ", " + temps.shared_ptr + ";");

    emit_line("add.u32 " + temps.offset_u32 + ", " + temps.addr + ", -" + hex_u32(opt.global_base_vaddr) + ";");
    emit_line("cvt.u64.u32 " + temps.global_ptr + ", " + temps.offset_u32 + ";");
    emit_line("add.u64 " + temps.global_ptr + ", " + rd(0) + ", " + temps.global_ptr + ";");
    return temps;
  }

  void emit_addr_map_and_ld_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + temps.is_shared + " ld.shared.u32 " + dst_r + ", [" + temps.shared_ptr + "];");
    emit_line("@" + temps.is_global + " ld.global.u32 " + dst_r + ", [" + temps.global_ptr + "];");
  }

  void emit_addr_map_and_ld_u32_leader(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_ld32_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_ld_u32(dst_r, addr_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_st_u32(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + temps.is_shared + " st.shared.u32 [" + temps.shared_ptr + "], " + src_r + ";");
    emit_line("@" + temps.is_global + " st.global.u32 [" + temps.global_ptr + "], " + src_r + ";");
  }

  void emit_addr_map_and_st_u32_leader(const std::string &addr_r, const std::string &src_r, uint32_t pc_for_err) {
    const std::string L_after = new_label("leader_st32_after");
    emit_line("@!" + p(0) + " bra " + L_after + ";");
    emit_addr_map_and_st_u32(addr_r, src_r, pc_for_err);
    emit_label(L_after);
  }

  void emit_addr_map_and_ld_u8_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
    const std::string byte_val = tmp_u8();
    emit_line("@" + temps.is_shared + " ld.shared.u8 " + byte_val + ", [" + temps.shared_ptr + "];");
    emit_line("@" + temps.is_global + " ld.global.u8 " + byte_val + ", [" + temps.global_ptr + "];");
    emit_line("cvt.u32.u8 " + dst_r + ", " + byte_val + ";");
  }

  void emit_addr_map_and_ld_u16_zext_u32(const std::string &dst_r, const std::string &addr_r, uint32_t pc_for_err) {
    const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
    const std::string half_val = tmp_u16();
    emit_line("@" + temps.is_shared + " ld.shared.u16 " + half_val + ", [" + temps.shared_ptr + "];");
    emit_line("@" + temps.is_global + " ld.global.u16 " + half_val + ", [" + temps.global_ptr + "];");
    emit_line("cvt.u32.u16 " + dst_r + ", " + half_val + ";");
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
    const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + temps.is_shared + " st.shared.u8 [" + temps.shared_ptr + "], " + src_u8 + ";");
    emit_line("@" + temps.is_global + " st.global.u8 [" + temps.global_ptr + "], " + src_u8 + ";");
  }

  void emit_addr_map_and_st_u16(const std::string &addr_r, const std::string &src_u16, uint32_t pc_for_err) {
    const AddrMapTemps temps = emit_prepare_addr_mapping(addr_r, pc_for_err);
    emit_line("@" + temps.is_shared + " st.shared.u16 [" + temps.shared_ptr + "], " + src_u16 + ";");
    emit_line("@" + temps.is_global + " st.global.u16 [" + temps.global_ptr + "], " + src_u16 + ";");
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
    const std::string dim = tmp_b32();
    emit_line("// builtin: " + std::string(kind) + "_id(dim=v0) -> v0");
    emit_line("mov.u32 " + dim + ", " + v(0) + ";"); // dim

    const std::string L_dim0 = new_label("dim0");
    const std::string L_dim1 = new_label("dim1");
    const std::string L_dim2 = new_label("dim2");
    const std::string L_done = new_label("dim_done");

    const std::string is_dim0 = tmp_pred();
    const std::string is_dim1 = tmp_pred();
    const std::string is_dim2 = tmp_pred();
    emit_line("setp.eq.u32 " + is_dim0 + ", " + dim + ", 0;");
    emit_line("@" + is_dim0 + " bra " + L_dim0 + ";");
    emit_line("setp.eq.u32 " + is_dim1 + ", " + dim + ", 1;");
    emit_line("@" + is_dim1 + " bra " + L_dim1 + ";");
    emit_line("setp.eq.u32 " + is_dim2 + ", " + dim + ", 2;");
    emit_line("@" + is_dim2 + " bra " + L_dim2 + ";");

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
        const std::string tid_r = tmp_b32();
        const std::string ntid_r = tmp_b32();
        const std::string ctaid_r = tmp_b32();
        const std::string gid_r = tmp_b32();
        emit_line("mov.u32 " + tid_r + ", " + std::string(tid) + ";");
        emit_line("mov.u32 " + ntid_r + ", " + std::string(ntid) + ";");
        emit_line("mov.u32 " + ctaid_r + ", " + std::string(ctaid) + ";");
        emit_line("mul.lo.u32 " + gid_r + ", " + ctaid_r + ", " + ntid_r + ";");
        emit_line("add.u32 " + gid_r + ", " + gid_r + ", " + tid_r + ";");
        emit_line("mov.u32 " + v(0) + ", " + gid_r + ";");
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
    const std::string dim = tmp_b32();
    emit_line("// builtin: global_size(dim=v0) -> v0");
    emit_line("mov.u32 " + dim + ", " + v(0) + ";"); // dim

    const std::string L_dim0 = new_label("gsize_dim0");
    const std::string L_dim1 = new_label("gsize_dim1");
    const std::string L_dim2 = new_label("gsize_dim2");
    const std::string L_done = new_label("gsize_done");

    const std::string is_dim0 = tmp_pred();
    const std::string is_dim1 = tmp_pred();
    const std::string is_dim2 = tmp_pred();
    emit_line("setp.eq.u32 " + is_dim0 + ", " + dim + ", 0;");
    emit_line("@" + is_dim0 + " bra " + L_dim0 + ";");
    emit_line("setp.eq.u32 " + is_dim1 + ", " + dim + ", 1;");
    emit_line("@" + is_dim1 + " bra " + L_dim1 + ";");
    emit_line("setp.eq.u32 " + is_dim2 + ", " + dim + ", 2;");
    emit_line("@" + is_dim2 + " bra " + L_dim2 + ";");

    // For out-of-range dims, return 1 (matches typical OpenCL behavior for unused dims).
    emit_line("mov.u32 " + v(0) + ", 1;");
    emit_line("bra " + L_done + ";");

    auto emit_dim = [&](const std::string &L, const char *ntid, const char *nctaid) {
      const std::string ntid_r = tmp_b32();
      const std::string nctaid_r = tmp_b32();
      const std::string size_r = tmp_b32();
      emit_label(L);
      emit_line("mov.u32 " + ntid_r + ", " + std::string(ntid) + ";");
      emit_line("mov.u32 " + nctaid_r + ", " + std::string(nctaid) + ";");
      emit_line("mul.lo.u32 " + size_r + ", " + ntid_r + ", " + nctaid_r + ";");
      emit_line("mov.u32 " + v(0) + ", " + size_r + ";");
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
    const std::string lhs = tmp_f32();
    const std::string rhs = tmp_f32();
    const std::string result = tmp_f32();
    const std::string lhs_nan = tmp_pred();
    const std::string rhs_nan = tmp_pred();
    emit_line("// builtin: fmax(v0,v1) -> v0 (f32 bits, NaN-safe)");
    emit_line("mov.b32 " + lhs + ", " + v(0) + ";");
    emit_line("mov.b32 " + rhs + ", " + v(1) + ";");
    emit_line("setp.nan.f32 " + lhs_nan + ", " + lhs + ", " + lhs + ";");
    emit_line("setp.nan.f32 " + rhs_nan + ", " + rhs + ", " + rhs + ";");
    emit_line("max.f32 " + result + ", " + lhs + ", " + rhs + ";");
    emit_line("selp.b32 " + result + ", " + rhs + ", " + result + ", " + lhs_nan + ";");
    emit_line("selp.b32 " + result + ", " + lhs + ", " + result + ", " + rhs_nan + ";");
    emit_line("mov.b32 " + v(0) + ", " + result + ";");
    (void)pc_for_err;
  }

  void emit_builtin_sqrtf(uint32_t pc_for_err) {
    const std::string src = tmp_f32();
    const std::string dst = tmp_f32();
    emit_line("// builtin: sqrtf(v0) -> v0 (f32 bits)");
    emit_line("mov.b32 " + src + ", " + v(0) + ";");
    emit_line("sqrt.rn.f32 " + dst + ", " + src + ";");
    emit_line("mov.b32 " + v(0) + ", " + dst + ";");
    (void)pc_for_err;
  }

  void emit_builtin_unary_f32_inplace(std::string_view opname, const std::string &dst_v, uint32_t pc_for_err) {
    const std::string src = tmp_f32();
    const std::string dst = tmp_f32();
    emit_line("mov.b32 " + src + ", " + dst_v + ";");
    if (opname == "sqrt") {
      emit_line("sqrt.rn.f32 " + dst + ", " + src + ";");
    } else if (opname == "cos") {
      emit_line("cos.approx.f32 " + dst + ", " + src + ";");
    } else if (opname == "sin") {
      emit_line("sin.approx.f32 " + dst + ", " + src + ";");
    } else {
      throw EmitError("unsupported.call", func_name, pc_for_err, "unary_op=" + std::string(opname));
    }
    emit_line("mov.b32 " + dst_v + ", " + dst + ";");
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
      const std::string src = tmp_f32();
      const std::string sin_v = tmp_f32();
      const std::string cos_v = tmp_f32();
      const std::string tan_v = tmp_f32();
      emit_line("mov.b32 " + src + ", " + v(i) + ";");
      emit_line("sin.approx.f32 " + sin_v + ", " + src + ";");
      emit_line("cos.approx.f32 " + cos_v + ", " + src + ";");
      emit_line("div.rn.f32 " + tan_v + ", " + sin_v + ", " + cos_v + ";");
      emit_line("mov.b32 " + v(i) + ", " + tan_v + ";");
    }
    (void)pc_for_err;
  }

  void emit_builtin_mad24iii(uint32_t pc_for_err) {
    // OpenCL: int mad24(int a, int b, int c) => mul24(a,b) + c (signed 24-bit multiply).
    // Calling convention (ventus clc): a=v0, b=v1, c=v2, ret=v0.
    const std::string lhs = tmp_b32();
    const std::string rhs = tmp_b32();
    const std::string acc = tmp_b32();
    emit_line("// builtin: mad24(v0,v1,v2) -> v0 (signed 24-bit)");
    emit_line("mov.b32 " + lhs + ", " + v(0) + ";");
    emit_line("mov.b32 " + rhs + ", " + v(1) + ";");
    emit_line("mov.b32 " + acc + ", " + v(2) + ";");
    // sign-extend low 24 bits: (x << 8) >> 8
    emit_line("shl.b32 " + lhs + ", " + lhs + ", 8;");
    emit_line("shr.s32 " + lhs + ", " + lhs + ", 8;");
    emit_line("shl.b32 " + rhs + ", " + rhs + ", 8;");
    emit_line("shr.s32 " + rhs + ", " + rhs + ", 8;");
    emit_line("mad.lo.s32 " + lhs + ", " + lhs + ", " + rhs + ", " + acc + ";");
    emit_line("mov.b32 " + v(0) + ", " + lhs + ";");
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
    const std::string bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc_for_err);
    const std::string sign_bits = tmp_b32();
    const std::string exp_bits = tmp_b32();
    const std::string frac_bits = tmp_b32();
    const std::string qnan_bit = tmp_b32();
    const std::string mask = tmp_b32();
    const std::string bit_mask = tmp_b32();
    const std::string is_sign = tmp_pred();
    const std::string is_exp_zero = tmp_pred();
    const std::string is_exp_all1 = tmp_pred();
    const std::string is_frac_zero = tmp_pred();
    const std::string is_zero = tmp_pred();
    const std::string is_sub = tmp_pred();
    const std::string is_inf = tmp_pred();
    const std::string is_nan = tmp_pred();
    const std::string is_norm = tmp_pred();
    const std::string is_qnan = tmp_pred();
    const std::string is_snan = tmp_pred();
    const std::string neg_class = tmp_pred();
    const std::string pos_class = tmp_pred();
    emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + bits + ", 0x80000000;");
    emit_line(scalar_prefix() + "and.b32 " + exp_bits + ", " + bits + ", 0x7f800000;");
    emit_line(scalar_prefix() + "and.b32 " + frac_bits + ", " + bits + ", 0x007fffff;");

    emit_line(scalar_prefix() + "setp.ne.u32 " + is_sign + ", " + sign_bits + ", 0;");
    emit_line(scalar_prefix() + "setp.eq.u32 " + is_exp_zero + ", " + exp_bits + ", 0;");
    emit_line(scalar_prefix() + "setp.eq.u32 " + is_exp_all1 + ", " + exp_bits + ", 0x7f800000;");
    emit_line(scalar_prefix() + "setp.eq.u32 " + is_frac_zero + ", " + frac_bits + ", 0;");

    emit_line(scalar_prefix() + "and.pred " + is_zero + ", " + is_exp_zero + ", " + is_frac_zero + ";");
    emit_line(scalar_prefix() + "not.pred " + is_sub + ", " + is_frac_zero + ";");
    emit_line(scalar_prefix() + "and.pred " + is_sub + ", " + is_exp_zero + ", " + is_sub + ";");
    emit_line(scalar_prefix() + "and.pred " + is_inf + ", " + is_exp_all1 + ", " + is_frac_zero + ";");
    emit_line(scalar_prefix() + "not.pred " + is_nan + ", " + is_frac_zero + ";");
    emit_line(scalar_prefix() + "and.pred " + is_nan + ", " + is_exp_all1 + ", " + is_nan + ";");
    emit_line(scalar_prefix() + "not.pred " + is_norm + ", " + is_exp_zero + ";");
    emit_line(scalar_prefix() + "not.pred " + is_qnan + ", " + is_exp_all1 + ";");
    emit_line(scalar_prefix() + "and.pred " + is_norm + ", " + is_norm + ", " + is_qnan + ";");

    emit_line(scalar_prefix() + "and.b32 " + qnan_bit + ", " + frac_bits + ", 0x00400000;");
    emit_line(scalar_prefix() + "setp.ne.u32 " + is_qnan + ", " + qnan_bit + ", 0;");
    emit_line(scalar_prefix() + "and.pred " + is_qnan + ", " + is_nan + ", " + is_qnan + ";");
    emit_line(scalar_prefix() + "not.pred " + is_snan + ", " + is_qnan + ";");
    emit_line(scalar_prefix() + "and.pred " + is_snan + ", " + is_nan + ", " + is_snan + ";");

    emit_line(scalar_prefix() + "mov.u32 " + mask + ", 0;");

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
      emit_line(scalar_prefix() + "selp.u32 " + bit_mask + ", " + std::to_string(bit) + ", 0, " + pred + ";");
      emit_line(scalar_prefix() + "or.b32 " + mask + ", " + mask + ", " + bit_mask + ";");
    };

    emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_inf + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_inf + ", " + pos_class + ";");
    or_if(neg_class, 1u);
    or_if(pos_class, 128u);

    emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_zero + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_zero + ", " + pos_class + ";");
    or_if(neg_class, 8u);
    or_if(pos_class, 16u);

    // -sub / +sub
    emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_sub + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_sub + ", " + pos_class + ";");
    or_if(neg_class, 4u);
    or_if(pos_class, 32u);

    // -norm / +norm
    emit_line(scalar_prefix() + "and.pred " + neg_class + ", " + is_norm + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "not.pred " + pos_class + ", " + is_sign + ";");
    emit_line(scalar_prefix() + "and.pred " + pos_class + ", " + is_norm + ", " + pos_class + ";");
    or_if(neg_class, 2u);
    or_if(pos_class, 64u);

    // NaNs
    or_if(is_snan, 256u);
    or_if(is_qnan, 512u);

    emit_st_x_u32_scalar(di.rd, mask, pc_for_err);
  }

  bool try_emit_scalar_fp(const sbt::DecodedInst &di) {
    if (!is_scalar_fp(di)) return false;
    const uint32_t pc = di.pc;
    require_uniform_pure_scalar(di, pc);

    // Bitwise moves (Zfinx: both sides are X regs).
    if (di.emit.scalar_fp_kind == ScalarFpKind::MoveBits) {
      emit_st_x_u32_scalar(di.rd, emit_ld_x_u32_scalar_tmp(di.rs1, pc), pc);
      return true;
    }

    // Sign injection.
    if (di.emit.scalar_fp_kind == ScalarFpKind::SignInject) {
      const std::string magnitude = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string sign_src = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
      const std::string result = tmp_b32();
      const std::string sign_bits = tmp_b32();
      emit_line(scalar_prefix() + "and.b32 " + result + ", " + magnitude + ", 0x7fffffff;");
      if (di.emit.fp_sign_inject_kind == FpSignInjectKind::CopySign) {
        emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + sign_src + ", 0x80000000;");
      } else if (di.emit.fp_sign_inject_kind == FpSignInjectKind::NegateSign) {
        emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + sign_src + ", 0x80000000;");
        emit_line(scalar_prefix() + "xor.b32 " + sign_bits + ", " + sign_bits + ", 0x80000000;");
      } else { // fsgnjx_s
        emit_line(scalar_prefix() + "xor.b32 " + sign_bits + ", " + magnitude + ", " + sign_src + ";");
        emit_line(scalar_prefix() + "and.b32 " + sign_bits + ", " + sign_bits + ", 0x80000000;");
      }
      emit_line(scalar_prefix() + "or.b32 " + result + ", " + result + ", " + sign_bits + ";");
      emit_st_x_u32_scalar(di.rd, result, pc);
      return true;
    }

    // Arithmetic.
    auto f32_binop = [&](const char *op) {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      const std::string lhs_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string rhs_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
      const std::string lhs = tmp_f32();
      const std::string rhs = tmp_f32();
      const std::string result = tmp_f32();
      const std::string result_bits = tmp_b32();
      emit_line(scalar_prefix() + "mov.b32 " + lhs + ", " + lhs_bits + ";");
      emit_line(scalar_prefix() + "mov.b32 " + rhs + ", " + rhs_bits + ";");
      emit_line(scalar_prefix() + std::string(op) + rm + ".f32 " + result + ", " + lhs + ", " + rhs + ";");
      emit_line(scalar_prefix() + "mov.b32 " + result_bits + ", " + result + ";");
      emit_st_x_u32_scalar(di.rd, result_bits, pc);
    };
    if (di.emit.scalar_fp_kind == ScalarFpKind::Binary) {
      switch (di.emit.fp_binary_kind) {
      case FpBinaryKind::Add: f32_binop("add"); return true;
      case FpBinaryKind::Sub: f32_binop("sub"); return true;
      case FpBinaryKind::Mul: f32_binop("mul"); return true;
      case FpBinaryKind::Div: f32_binop("div"); return true;
      case FpBinaryKind::None: break;
      }
    }
    if (di.emit.scalar_fp_kind == ScalarFpKind::Sqrt) {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      const std::string src_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string src = tmp_f32();
      const std::string dst = tmp_f32();
      const std::string dst_bits = tmp_b32();
      emit_line(scalar_prefix() + "mov.b32 " + src + ", " + src_bits + ";");
      emit_line(scalar_prefix() + "sqrt" + rm + ".f32 " + dst + ", " + src + ";");
      emit_line(scalar_prefix() + "mov.b32 " + dst_bits + ", " + dst + ";");
      emit_st_x_u32_scalar(di.rd, dst_bits, pc);
      return true;
    }

    // FMA family.
    if (di.emit.scalar_fp_kind == ScalarFpKind::Fma) {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      const std::string a_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string b_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
      const std::string c_bits = emit_ld_x_u32_scalar_tmp(di.rs3, pc);
      const std::string a = tmp_f32();
      const std::string b = tmp_f32();
      const std::string c = tmp_f32();
      const std::string dst = tmp_f32();
      const std::string dst_bits = tmp_b32();
      if (di.emit.fp_ternary_kind == FpTernaryKind::MSub || di.emit.fp_ternary_kind == FpTernaryKind::NMAdd) {
        emit_line(scalar_prefix() + "xor.b32 " + c_bits + ", " + c_bits + ", 0x80000000;");
      }
      if (di.emit.fp_ternary_kind == FpTernaryKind::NMSub || di.emit.fp_ternary_kind == FpTernaryKind::NMAdd) {
        emit_line(scalar_prefix() + "xor.b32 " + a_bits + ", " + a_bits + ", 0x80000000;");
      }
      emit_line(scalar_prefix() + "mov.b32 " + a + ", " + a_bits + ";");
      emit_line(scalar_prefix() + "mov.b32 " + b + ", " + b_bits + ";");
      emit_line(scalar_prefix() + "mov.b32 " + c + ", " + c_bits + ";");
      emit_line(scalar_prefix() + "fma" + rm + ".f32 " + dst + ", " + a + ", " + b + ", " + c + ";");
      emit_line(scalar_prefix() + "mov.b32 " + dst_bits + ", " + dst + ";");
      emit_st_x_u32_scalar(di.rd, dst_bits, pc);
      return true;
    }

    // Min/max (NaN-safe).
    if (di.emit.scalar_fp_kind == ScalarFpKind::MinMax) {
      const std::string lhs_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string rhs_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
      const std::string lhs = tmp_f32();
      const std::string rhs = tmp_f32();
      const std::string result = tmp_f32();
      const std::string lhs_nan = tmp_pred();
      const std::string rhs_nan = tmp_pred();
      const std::string result_bits = tmp_b32();
      emit_line(scalar_prefix() + "mov.b32 " + lhs + ", " + lhs_bits + ";");
      emit_line(scalar_prefix() + "mov.b32 " + rhs + ", " + rhs_bits + ";");
      emit_line(scalar_prefix() + "setp.nan.f32 " + lhs_nan + ", " + lhs + ", " + lhs + ";");
      emit_line(scalar_prefix() + "setp.nan.f32 " + rhs_nan + ", " + rhs + ", " + rhs + ";");
      emit_line(scalar_prefix() + std::string(di.emit.fp_minmax_kind == FpMinMaxKind::Max ? "max" : "min") + ".f32 " + result + ", " + lhs +
                ", " + rhs + ";");
      emit_line(scalar_prefix() + "selp.b32 " + result + ", " + rhs + ", " + result + ", " + lhs_nan + ";");
      emit_line(scalar_prefix() + "selp.b32 " + result + ", " + lhs + ", " + result + ", " + rhs_nan + ";");
      emit_line(scalar_prefix() + "mov.b32 " + result_bits + ", " + result + ";");
      emit_st_x_u32_scalar(di.rd, result_bits, pc);
      return true;
    }

    // Comparisons: exact 0/1 integer result.
    if (di.emit.scalar_fp_kind == ScalarFpKind::Compare) {
      const std::string lhs_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string rhs_bits = emit_ld_x_u32_scalar_tmp(di.rs2, pc);
      const std::string lhs = tmp_f32();
      const std::string rhs = tmp_f32();
      const std::string pred = tmp_pred();
      const std::string result = tmp_b32();
      emit_line(scalar_prefix() + "mov.b32 " + lhs + ", " + lhs_bits + ";");
      emit_line(scalar_prefix() + "mov.b32 " + rhs + ", " + rhs_bits + ";");
      if (di.emit.fp_compare_kind == FpCompareKind::Eq) emit_line(scalar_prefix() + "setp.eq.f32 " + pred + ", " + lhs + ", " + rhs + ";");
      else if (di.emit.fp_compare_kind == FpCompareKind::Lt) emit_line(scalar_prefix() + "setp.lt.f32 " + pred + ", " + lhs + ", " + rhs + ";");
      else emit_line(scalar_prefix() + "setp.le.f32 " + pred + ", " + lhs + ", " + rhs + ";");
      emit_line(scalar_prefix() + "selp.u32 " + result + ", 1, 0, " + pred + ";");
      emit_st_x_u32_scalar(di.rd, result, pc);
      return true;
    }

    // Conversions.
    if (di.emit.scalar_fp_kind == ScalarFpKind::Convert && (di.emit.fp_convert_kind == FpConvertKind::IntToFloatSigned ||
                                                             di.emit.fp_convert_kind == FpConvertKind::IntToFloatUnsigned)) {
      const std::string rm = ptx_rm_f32(di.fp_rm, pc);
      const std::string src = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string dst = tmp_f32();
      const std::string dst_bits = tmp_b32();
      emit_line(scalar_prefix() + "cvt" + rm + ".f32." +
                (di.emit.fp_convert_kind == FpConvertKind::IntToFloatSigned ? "s32 " : "u32 ") + dst + ", " + src + ";");
      emit_line(scalar_prefix() + "mov.b32 " + dst_bits + ", " + dst + ";");
      emit_st_x_u32_scalar(di.rd, dst_bits, pc);
      return true;
    }
    if (di.emit.scalar_fp_kind == ScalarFpKind::Convert && (di.emit.fp_convert_kind == FpConvertKind::FloatToIntSigned ||
                                                             di.emit.fp_convert_kind == FpConvertKind::FloatToIntUnsigned)) {
      const std::string rm = ptx_rm_cvt_i32(di.fp_rm, pc);
      const std::string src_bits = emit_ld_x_u32_scalar_tmp(di.rs1, pc);
      const std::string src = tmp_f32();
      const std::string dst = tmp_b32();
      emit_line(scalar_prefix() + "mov.b32 " + src + ", " + src_bits + ";");
      emit_line(scalar_prefix() + "cvt" + rm + "." +
                (di.emit.fp_convert_kind == FpConvertKind::FloatToIntSigned ? "s32" : "u32") + ".f32 " + dst + ", " + src + ";");
      emit_st_x_u32_scalar(di.rd, dst, pc);
      return true;
    }

    if (di.emit.scalar_fp_kind == ScalarFpKind::Classify) {
      emit_scalar_fclass_s(di, pc);
      return true;
    }

    throw EmitError("unsupported.inst", func_name, pc, di.name);
  }

  std::string mma_detail(const sbt::MmaInstInfo &mma) const {
    return "shape=" + std::string(sbt::to_string(mma.shape)) + " layout=" + std::string(sbt::to_string(mma.a_layout)) + "." +
           std::string(sbt::to_string(mma.b_layout)) + " ab=" + std::string(sbt::to_string(mma.ab_type)) + " cd=" +
           std::string(sbt::to_string(mma.cd_type)) + " support=" + std::string(sbt::to_string(mma.support_class)) + " lowering=" +
           std::string(sbt::to_string(mma.lowering_class));
  }

  void emit_compute_mma_scratch_base(const std::string &dst_rd, uint32_t pc_for_err) {
    require(opt.stack_stride_bytes >= 1024u,
            EmitError("unsupported.mma.stack_stride", func_name, pc_for_err, "stack_stride_bytes<1024"));
    const std::string scratch_off = tmp_b32();
    const std::string scratch_off_rd = tmp_b64();
    emit_line("mul.lo.u32 " + scratch_off + ", " + r(10) + ", " + std::to_string(opt.stack_stride_bytes) + ";");
    emit_line("cvt.u64.u32 " + scratch_off_rd + ", " + scratch_off + ";");
    emit_line("add.u64 " + dst_rd + ", " + rd(2) + ", " + scratch_off_rd + ";");
  }

  void emit_load_u32_from_mma_scratch(const std::string &dst_r, const std::string &scratch_rd, const std::string &reg_r,
                                      const std::string &lane_r, const std::string &off_r) {
    const std::string lane_bytes = tmp_b32();
    const std::string addr_rd = tmp_b64();
    emit_line("mul.lo.u32 " + off_r + ", " + reg_r + ", 128;");
    emit_line("shl.b32 " + lane_bytes + ", " + lane_r + ", 2;");
    emit_line("add.u32 " + off_r + ", " + off_r + ", " + lane_bytes + ";");
    emit_line("cvt.u64.u32 " + addr_rd + ", " + off_r + ";");
    emit_line("add.u64 " + addr_rd + ", " + scratch_rd + ", " + addr_rd + ";");
    emit_line("ld.shared.u32 " + dst_r + ", [" + addr_rd + "];");
  }

  void emit_store_u32_to_mma_scratch(const std::string &scratch_rd, const std::string &reg_r, const std::string &lane_r,
                                     const std::string &src_r, const std::string &off_r) {
    const std::string lane_bytes = tmp_b32();
    const std::string addr_rd = tmp_b64();
    emit_line("mul.lo.u32 " + off_r + ", " + reg_r + ", 128;");
    emit_line("shl.b32 " + lane_bytes + ", " + lane_r + ", 2;");
    emit_line("add.u32 " + off_r + ", " + off_r + ", " + lane_bytes + ";");
    emit_line("cvt.u64.u32 " + addr_rd + ", " + off_r + ";");
    emit_line("add.u64 " + addr_rd + ", " + scratch_rd + ", " + addr_rd + ";");
    emit_line("st.shared.u32 [" + addr_rd + "], " + src_r + ";");
  }

  void emit_spill_v_window_to_mma_scratch(const std::string &scratch_rd, int base_reg, uint8_t reg_count) {
    for (uint8_t reg = 0; reg < reg_count; ++reg) {
      const std::string reg_idx = tmp_b32();
      const std::string value = tmp_b32();
      const std::string off = tmp_b32();
      emit_line("mov.u32 " + reg_idx + ", " + std::to_string(reg) + ";");
      emit_line("mov.u32 " + value + ", " + v(base_reg + static_cast<int>(reg)) + ";");
      emit_store_u32_to_mma_scratch(scratch_rd, reg_idx, r(0), value, off);
    }
    emit_warp_sync();
  }

  void emit_reload_v_window_from_mma_scratch(const std::string &scratch_rd, int base_reg, uint8_t reg_count) {
    for (uint8_t reg = 0; reg < reg_count; ++reg) {
      const std::string reg_idx = tmp_b32();
      const std::string value = tmp_b32();
      const std::string off = tmp_b32();
      emit_line("mov.u32 " + reg_idx + ", " + std::to_string(reg) + ";");
      emit_load_u32_from_mma_scratch(value, scratch_rd, reg_idx, r(0), off);
      emit_line("mov.u32 " + v(base_reg + static_cast<int>(reg)) + ", " + value + ";");
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

    if ((role == sbt::ptx::mma::OperandRole::C || role == sbt::ptx::mma::OperandRole::D) &&
        sbt::ptx::mma::tuple_pack(abi, role) == sbt::ptx::mma::PackMode::Packed16x2) {
      const std::string take_hi = tmp_pred();
      emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 6;");
      emit_line("shr.u32 " + lane_r + ", " + idx_r + ", 1;");
      emit_line("and.b32 " + lane_r + ", " + lane_r + ", 31;");
      emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
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
        emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
        return;
      }
      emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 6;");
      emit_line("shr.u32 " + lane_r + ", " + idx_r + ", 1;");
      emit_line("and.b32 " + lane_r + ", " + lane_r + ", 31;");
      emit_load_u32_from_mma_scratch(dst_r, scratch_rd, reg_r, lane_r, tmp_r);
      emit_line("and.b32 " + tmp2_r + ", " + idx_r + ", 1;");
      const std::string take_hi = tmp_pred();
      emit_line("setp.ne.u32 " + take_hi + ", " + tmp2_r + ", 0;");
      emit_line("@" + take_hi + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
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
    const std::string take_hi = tmp_pred();
    emit_line("setp.ne.u32 " + take_hi + ", " + tmp2_r + ", 0;");
    emit_line("@" + take_hi + " shr.u32 " + dst_r + ", " + dst_r + ", 16;");
    emit_line("and.b32 " + dst_r + ", " + dst_r + ", 0xffff;");
  }

  void emit_store_scalar_value_to_spilled_cd_window(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset,
                                                    const sbt::ptx::mma::ScalarTupleValue &value, const std::string &scratch_rd,
                                                    const std::string &src_r,
                                                    const std::string &row_r, const std::string &col_r, const std::string &idx_r,
                                                    const std::string &reg_r, const std::string &lane_r, const std::string &tmp_r) {
    emit_compute_tuple_logical_coord(value, row_r, col_r);
    emit_compute_window_index_from_logical_coord(di, sbt::ptx::mma::OperandRole::D, slice_col_offset, row_r, col_r, idx_r, tmp_r);

    if (sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D) == sbt::ptx::mma::PackMode::Packed16x2) {
      const std::string take_hi = tmp_pred();
      emit_line("and.b32 " + src_r + ", " + src_r + ", 0xffff;");
      emit_line("shr.u32 " + reg_r + ", " + idx_r + ", 6;");
      emit_line("shr.u32 " + lane_r + ", " + idx_r + ", 1;");
      emit_line("and.b32 " + lane_r + ", " + lane_r + ", 31;");
      emit_load_u32_from_mma_scratch(tmp_r, scratch_rd, reg_r, lane_r, row_r);
      emit_line("and.b32 " + col_r + ", " + idx_r + ", 1;");
      emit_line("setp.ne.u32 " + take_hi + ", " + col_r + ", 0;");
      emit_line("@!" + take_hi + " and.b32 " + tmp_r + ", " + tmp_r + ", 0xffff0000;");
      emit_line("@!" + take_hi + " or.b32 " + tmp_r + ", " + tmp_r + ", " + src_r + ";");
      emit_line("@" + take_hi + " and.b32 " + tmp_r + ", " + tmp_r + ", 0x0000ffff;");
      emit_line("@" + take_hi + " shl.b32 " + row_r + ", " + src_r + ", 16;");
      emit_line("@" + take_hi + " or.b32 " + tmp_r + ", " + tmp_r + ", " + row_r + ";");
      emit_store_u32_to_mma_scratch(scratch_rd, reg_r, lane_r, tmp_r, row_r);
      return;
    }

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
        const std::string lo_word = tmp_b32();
        const std::string hi_word = tmp_b32();
        for (uint8_t elem = 0; elem < 2u; ++elem) {
          const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, role, tuple_reg, elem);
          const std::string dst_word = (elem == 0u) ? lo_word : hi_word;
          emit_load_scalar_value_from_spilled_window(di, abi, role, slice_col_offset, value, scratch_rd, dst_word, tmp_b32(), tmp_b32(), tmp_b32(),
                                                     tmp_b32(), tmp_b32(), tmp_b32(), tmp_b32());
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
      emit_load_scalar_value_from_spilled_window(di, abi, role, slice_col_offset, value, scratch_rd, value_word, tmp_b32(), tmp_b32(), tmp_b32(),
                                                 tmp_b32(), tmp_b32(), tmp_b32(), tmp_b32());
      if (role == sbt::ptx::mma::OperandRole::C || role == sbt::ptx::mma::OperandRole::D) emit_line("mov.b32 " + dst_regs[tuple_reg] + ", " + value_word + ";");
      else emit_line("mov.u32 " + dst_regs[tuple_reg] + ", " + value_word + ";");
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
        const std::string lo_word = tmp_b32();
        const std::string hi_word = tmp_b32();
        for (uint8_t elem = 0; elem < 2u; ++elem) {
          const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::B, tuple_reg, elem);
          const std::string dst_word = (elem == 0u) ? lo_word : hi_word;
          emit_load_b_scalar_value_from_spilled_window(di, abi, plan, value, scratch_rd, dst_word, tmp_b32(), tmp_b32(), tmp_b32(), tmp_b32(),
                                                       tmp_b32(), tmp_b32(), tmp_b32());
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
      emit_load_b_scalar_value_from_spilled_window(di, abi, plan, value, scratch_rd, value_word, tmp_b32(), tmp_b32(), tmp_b32(), tmp_b32(),
                                                   tmp_b32(), tmp_b32(), tmp_b32());
      emit_line("mov.u32 " + dst_regs[tuple_reg] + ", " + value_word + ";");
    }
  }

  void emit_store_d_tuple_to_spilled_cd_window(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset,
                                               const std::string &scratch_rd, const std::vector<std::string> &src_regs) {
    const auto pack = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D);
    const uint8_t reg_count = sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::D);
    require(src_regs.size() == reg_count, EmitError("invalid.mma.abi", func_name, di.pc, mma_detail(di.mma)));

    if (pack == sbt::ptx::mma::PackMode::Packed16x2) {
      for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
        const std::string lo_half = tmp_b16();
        const std::string hi_half = tmp_b16();
        const std::string half_word = tmp_u16();
        const std::string src_word = tmp_b32();
        emit_line("mov.b32 {" + lo_half + ", " + hi_half + "}, " + src_regs[tuple_reg] + ";");
        for (uint8_t elem = 0; elem < 2u; ++elem) {
          const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::D, tuple_reg, elem);
          const std::string elem_half = (elem == 0u) ? lo_half : hi_half;
          emit_line("mov.b16 " + half_word + ", " + elem_half + ";");
          emit_line("cvt.u32.u16 " + src_word + ", " + half_word + ";");
          emit_store_scalar_value_to_spilled_cd_window(di, abi, slice_col_offset, value, scratch_rd, src_word, tmp_b32(), tmp_b32(), tmp_b32(),
                                                       tmp_b32(), tmp_b32(), tmp_b32());
        }
      }
      return;
    }

    for (uint8_t tuple_reg = 0; tuple_reg < reg_count; ++tuple_reg) {
      const auto value = sbt::ptx::mma::scalar_tuple_plan(abi, sbt::ptx::mma::OperandRole::D, tuple_reg, 0u);
      const std::string src_word = tmp_b32();
      emit_line("mov.b32 " + src_word + ", " + src_regs[tuple_reg] + ";");
      emit_store_scalar_value_to_spilled_cd_window(di, abi, slice_col_offset, value, scratch_rd, src_word, tmp_b32(), tmp_b32(), tmp_b32(),
                                                   tmp_b32(), tmp_b32(), tmp_b32());
    }
  }

  void validate_native_mma_contract(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi) {
    require(sbt::ptx::mma::shape_m(di.mma) == abi.m, EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
    require(sbt::ptx::mma::shape_k(di.mma) == abi.k, EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
    require(sbt::ptx::mma::shape_n(di.mma) == abi.n, EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
    const uint8_t cd_carrier_regs =
        (di.mma.cd_type == sbt::MmaCdType::Fp16) ? static_cast<uint8_t>(di.mma.c_regs_per_thread / 2u) : di.mma.c_regs_per_thread;
    require(cd_carrier_regs == sbt::ptx::mma::tuple_reg_count(abi, sbt::ptx::mma::OperandRole::C),
            EmitError("invalid.mma.native_contract", func_name, di.pc, mma_detail(di.mma)));
  }

  void validate_split_n_contract(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi) {
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

  void emit_native_mma_sync(const sbt::DecodedInst &di, const sbt::ptx::mma::AbiDesc &abi, uint8_t slice_col_offset) {
    std::vector<std::string> a_tuple_regs;
    a_tuple_regs.reserve(kMmaATupleRegIds.size());
    for (size_t i = 0; i < kMmaATupleRegIds.size(); ++i) a_tuple_regs.push_back(tmp_b32());

    std::vector<std::string> b_tuple_regs;
    b_tuple_regs.reserve(kMmaBTupleRegIds.size());
    for (size_t i = 0; i < kMmaBTupleRegIds.size(); ++i) b_tuple_regs.push_back(tmp_b32());

    const std::string scratch_base = tmp_b64();
    emit_compute_mma_scratch_base(scratch_base, di.pc);

    emit_spill_v_window_to_mma_scratch(scratch_base, di.mma.rs1_base, di.mma.a_regs_per_thread);
    emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::A, 0u, scratch_base, a_tuple_regs);

    const auto b_plan = sbt::ptx::mma::b_source_window_plan(di.mma, abi, slice_col_offset);
    emit_spill_v_window_to_mma_scratch(scratch_base, di.mma.rs2_base + static_cast<int>(b_plan.reg_offset), b_plan.reg_count);
    emit_materialize_b_tuple_regs(di, abi, b_plan, scratch_base, b_tuple_regs);

    const uint8_t cd_carrier_regs =
        (di.mma.cd_type == sbt::MmaCdType::Fp16) ? static_cast<uint8_t>(di.mma.c_regs_per_thread / 2u) : di.mma.c_regs_per_thread;
    emit_spill_v_window_to_mma_scratch(scratch_base, di.mma.rd_base, cd_carrier_regs);
    const bool packed_d = sbt::ptx::mma::tuple_pack(abi, sbt::ptx::mma::OperandRole::D) == sbt::ptx::mma::PackMode::Packed16x2;
    if (packed_d) {
      const std::string c0 = tmp_b32();
      const std::string c1 = tmp_b32();
      const std::string d0 = tmp_b32();
      const std::string d1 = tmp_b32();
      emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::C, slice_col_offset, scratch_base, {c0, c1});
      emit_line(std::string(abi.ptx_opcode) + " {" + d0 + ", " + d1 + "}, {" + a_tuple_regs[0] + ", " + a_tuple_regs[1] + ", " +
                a_tuple_regs[2] + ", " + a_tuple_regs[3] + "}, {" + b_tuple_regs[0] + ", " + b_tuple_regs[1] + "}, {" + c0 + ", " + c1 + "};");
      emit_store_d_tuple_to_spilled_cd_window(di, abi, slice_col_offset, scratch_base, {d0, d1});
    } else {
      const std::string c0 = tmp_f32();
      const std::string c1 = tmp_f32();
      const std::string c2 = tmp_f32();
      const std::string c3 = tmp_f32();
      const std::string d0 = tmp_f32();
      const std::string d1 = tmp_f32();
      const std::string d2 = tmp_f32();
      const std::string d3 = tmp_f32();
      emit_materialize_tuple_regs(di, abi, sbt::ptx::mma::OperandRole::C, slice_col_offset, scratch_base, {c0, c1, c2, c3});
      emit_line(std::string(abi.ptx_opcode) + " {" + d0 + ", " + d1 + ", " + d2 + ", " + d3 + "}, {" + a_tuple_regs[0] + ", " +
                a_tuple_regs[1] + ", " + a_tuple_regs[2] + ", " + a_tuple_regs[3] + "}, {" + b_tuple_regs[0] + ", " + b_tuple_regs[1] + "}, {" +
                c0 + ", " + c1 + ", " + c2 + ", " + c3 + "};");
      emit_store_d_tuple_to_spilled_cd_window(di, abi, slice_col_offset, scratch_base, {d0, d1, d2, d3});
    }

    emit_warp_sync();
    emit_reload_v_window_from_mma_scratch(scratch_base, di.mma.rd_base, cd_carrier_regs);
  }

  void emit_mma_inst(const sbt::DecodedInst &di) {
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

  void emit_one_inst(const sbt::cfg::BundleInst &bi) {
    const sbt::DecodedInst &di = bi.inst;
    const uint32_t pc = di.pc;
    const uint32_t bundle_pc = bi.pc;
    const uint32_t inst_pc = bi.inst_pc;

    if (opt.include_comments) {
      emit_line("// " + hex_u32(bi.pc) + " " + di.name);
    }

    // Structured control in the emitter contract.
    if (di.emit.domain == EmitDomain::StructuredControl) {
      if (di.emit.structured_control_kind == StructuredControlKind::SetRpc ||
          di.emit.structured_control_kind == StructuredControlKind::Join ||
          di.emit.structured_control_kind == StructuredControlKind::Vsetvli) {
        return;
      }
      if (di.emit.structured_control_kind == StructuredControlKind::EndPrg) {
        if (is_entry) {
          emit_entry_pds_pool_release(pc);
        }
        if (!is_entry) emit_store_mutable_state_blob("__sbt_mutable_state_out");
        emit_line("ret;");
        return;
      }
      if (di.emit.structured_control_kind == StructuredControlKind::Barrier) {
        emit_line("bar.sync 0;");
        return;
      }
    }

    if (is_ret(di)) {
      if (is_entry) {
        emit_entry_pds_pool_release(pc);
      }
      if (!is_entry) emit_store_mutable_state_blob("__sbt_mutable_state_out");
      emit_line("ret;");
      return;
    }

    if (sbt::is_scalar_exec_classification_required(di)) {
      (void)scalar_exec_kind_for_inst(di, func_name);
    }

    // Calls: support a small inlined builtin set, plus direct calls to emitted `.func`s.
    if (is_call(di)) {
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
      auto it = sym_by_addr.find(target);
      require(it != sym_by_addr.end(), EmitError("unsupported.call", func_name, pc, "target=" + hex_u32(target)));
      const std::string &callee = it->second;

      const std::string ret_addr = tmp_b32();
      emit_line("mov.u32 " + ret_addr + ", " + hex_u32(inst_pc + 4) + ";");
      emit_st_x_u32_scalar(di.rd, ret_addr, pc);

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
          const std::string tid = tmp_b32();
          const std::string ntid = tmp_b32();
          const std::string ctaid = tmp_b32();
          const std::string gid = tmp_b32();
          emit_line("mov.u32 " + tid + ", %tid.x;");
          emit_line("mov.u32 " + ntid + ", %ntid.x;");
          emit_line("mov.u32 " + ctaid + ", %ctaid.x;");
          emit_line("mul.lo.u32 " + gid + ", " + ctaid + ", " + ntid + ";");
          emit_line("add.u32 " + gid + ", " + gid + ", " + tid + ";");
          emit_line("mov.u32 " + v(0) + ", " + gid + ";");
        } else if (callee == "__builtin_riscv_global_id_y") {
          const std::string tid = tmp_b32();
          const std::string ntid = tmp_b32();
          const std::string ctaid = tmp_b32();
          const std::string gid = tmp_b32();
          emit_line("mov.u32 " + tid + ", %tid.y;");
          emit_line("mov.u32 " + ntid + ", %ntid.y;");
          emit_line("mov.u32 " + ctaid + ", %ctaid.y;");
          emit_line("mul.lo.u32 " + gid + ", " + ctaid + ", " + ntid + ";");
          emit_line("add.u32 " + gid + ", " + gid + ", " + tid + ";");
          emit_line("mov.u32 " + v(0) + ", " + gid + ";");
        } else if (callee == "__builtin_riscv_global_id_z") {
          const std::string tid = tmp_b32();
          const std::string ntid = tmp_b32();
          const std::string ctaid = tmp_b32();
          const std::string gid = tmp_b32();
          emit_line("mov.u32 " + tid + ", %tid.z;");
          emit_line("mov.u32 " + ntid + ", %ntid.z;");
          emit_line("mov.u32 " + ctaid + ", %ctaid.z;");
          emit_line("mul.lo.u32 " + gid + ", " + ctaid + ", " + ntid + ";");
          emit_line("add.u32 " + gid + ", " + gid + ", " + tid + ";");
          emit_line("mov.u32 " + v(0) + ", " + gid + ";");
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
    if (is_scalar_branch(di) && di.imm_kind == sbt::ImmKind::B13) {
      require_uniform_pure_scalar(di, pc);
      const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(inst_pc) + static_cast<int64_t>(di.imm));
      const uint32_t fallthrough = inst_pc + 4;
      const uint32_t src_block = block_start_of_pc(bundle_pc);
      const uint32_t dst_t = block_start_of_pc(target);
      const uint32_t dst_f = block_start_of_pc(fallthrough);

      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_ld_x_u32_all(r(15), di.rs2, pc);

      switch (di.emit.branch_cond) {
      case BranchCondKind::Eq: emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
      case BranchCondKind::Ne: emit_line("setp.ne.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
      case BranchCondKind::Lt: emit_line("setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
      case BranchCondKind::Ge: emit_line("setp.ge.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
      case BranchCondKind::Ltu: emit_line("setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
      case BranchCondKind::Geu: emit_line("setp.ge.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";"); break;
      case BranchCondKind::None: throw EmitError("unsupported.inst", func_name, pc, di.name);
      }

      emit_line("@" + p(1) + " bra.uni " + target_label_for_edge(src_block, dst_t) + ";");
      emit_line("bra.uni " + target_label_for_edge(src_block, dst_f) + ";");
      return;
    }

    // Vector conditional branches.
    if (is_vector_branch(di) && di.imm_kind == sbt::ImmKind::B13) {
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

      switch (di.emit.branch_cond) {
      case BranchCondKind::Eq: emit_line("setp.eq.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
      case BranchCondKind::Ne: emit_line("setp.ne.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
      case BranchCondKind::Lt: emit_line("setp.lt.s32 " + p(1) + ", " + a + ", " + b + ";"); break;
      case BranchCondKind::Ge: emit_line("setp.ge.s32 " + p(1) + ", " + a + ", " + b + ";"); break;
      case BranchCondKind::Ltu: emit_line("setp.lt.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
      case BranchCondKind::Geu: emit_line("setp.ge.u32 " + p(1) + ", " + a + ", " + b + ";"); break;
      case BranchCondKind::None: throw EmitError("unsupported.inst", func_name, pc, di.name);
      }

      emit_line("@" + p(1) + " bra " + target_label_for_edge(src_block, dst_t) + ";");
      emit_line("bra " + target_label_for_edge(src_block, dst_f) + ";");
      return;
    }

    // CSR ops (prototype: treat Ventus CSRs as read-only for bring-up).
    if (di.emit.domain == EmitDomain::Csr) {
      require_uniform_pure_scalar(di, pc);
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
      } else {
        throw EmitError("unsupported.csr", func_name, pc, "csr=" + hex_u32(csr));
      }

      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }

    // Scalar loads/stores (uniform ops).
    if (is_scalar_memory(di, MemAccessKind::Load) && di.imm_kind == sbt::ImmKind::I12) {
      require_uniform_pure_scalar(di, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "add.u32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");

      // Scalar loads.
      if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::Integer) {
        emit_addr_map_and_ld_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::FloatBits) {
        // Zfinx model: flw loads f32 bits into an X reg.
        emit_addr_map_and_ld_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
        emit_addr_map_and_ld_u8_zext_u32_scalar(r(17), r(15), pc);
        emit_line(scalar_prefix() + "shl.b32 " + r(17) + ", " + r(17) + ", 24;");
        emit_line(scalar_prefix() + "shr.s32 " + r(17) + ", " + r(17) + ", 24;");
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
        emit_addr_map_and_ld_u16_zext_u32_scalar(r(17), r(15), pc);
        emit_line(scalar_prefix() + "shl.b32 " + r(17) + ", " + r(17) + ", 16;");
        emit_line(scalar_prefix() + "shr.s32 " + r(17) + ", " + r(17) + ", 16;");
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
        emit_addr_map_and_ld_u8_zext_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
        emit_addr_map_and_ld_u16_zext_u32_scalar(r(17), r(15), pc);
        emit_st_x_u32_scalar(di.rd, r(17), pc);
        return;
      }
      throw EmitError("unsupported.inst", func_name, pc, di.name);
    }

    if (is_scalar_memory(di, MemAccessKind::Store) && di.imm_kind == sbt::ImmKind::S12) {
      require_scalar_exec_kind(di, ScalarExecKind::ExternallySideEffecting, pc);
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // base
      emit_ld_x_u32_scalar(r(15), di.rs2, pc); // value
      emit_line(scalar_prefix() + "add.u32 " + r(16) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");

      // Scalar stores.
      if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::Integer) {
        emit_addr_map_and_st_u32_scalar(r(16), r(15), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Word && di.emit.mem_value_kind == MemValueKind::FloatBits) {
        // Zfinx model: fsw stores f32 bits from an X reg.
        emit_addr_map_and_st_u32_scalar(r(16), r(15), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Byte) {
        emit_line(scalar_prefix() + "cvt.u8.u32 " + u8(1) + ", " + r(15) + ";");
        emit_addr_map_and_st_u8_scalar(r(16), u8(1), pc);
        return;
      }
      if (di.emit.mem_width == MemWidth::Half) {
        emit_line(scalar_prefix() + "cvt.u16.u32 " + u16(1) + ", " + r(15) + ";");
        emit_addr_map_and_st_u16_scalar(r(16), u16(1), pc);
        return;
      }
      throw EmitError("unsupported.inst", func_name, pc, di.name);
    }

    // Scalar FP ops (Zfinx: f32 bits carried in X regs).
    if (try_emit_scalar_fp(di)) return;

    if (is_scalar_int(di)) {
      require_uniform_pure_scalar(di, pc);
    }

    // Scalar ALU ops (uniform) - minimal subset used by Rodinia.
    if (di.emit.scalar_int_kind == ScalarIntKind::Add && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "add.s32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Add && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "add.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Sub) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "sub.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::And && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Or && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "or.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Xor && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "xor.b32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::And && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(15) + ", " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Or && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "or.b32 " + r(15) + ", " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Mul) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mul.lo.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::MulH) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mul.hi.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::MulHU) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "mul.hi.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::MulHSU) {
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
    if (di.emit.scalar_int_kind == ScalarIntKind::Div || di.emit.scalar_int_kind == ScalarIntKind::DivU ||
        di.emit.scalar_int_kind == ScalarIntKind::Rem || di.emit.scalar_int_kind == ScalarIntKind::RemU) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc); // dividend
      emit_ld_x_u32_scalar(r(15), di.rs2, pc); // divisor

      const bool signed_div = (di.emit.scalar_int_kind == ScalarIntKind::Div || di.emit.scalar_int_kind == ScalarIntKind::Rem);
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

      if (di.emit.scalar_int_kind == ScalarIntKind::Div) {
        emit_line("mov.u32 " + r(16) + ", 0xffffffff;"); // div by zero => -1
        emit_line("@" + p(2) + " mov.u32 " + r(16) + ", 0x80000000;"); // overflow => INT_MIN
        emit_line("@" + p(4) + " div.s32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      } else if (di.emit.scalar_int_kind == ScalarIntKind::DivU) {
        emit_line("mov.u32 " + r(16) + ", 0xffffffff;"); // div by zero => all-ones
        emit_line("@" + p(4) + " div.u32 " + r(16) + ", " + r(14) + ", " + r(15) + ";");
      } else if (di.emit.scalar_int_kind == ScalarIntKind::Rem) {
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
    if (di.emit.scalar_int_kind == ScalarIntKind::Lui && di.imm_kind == sbt::ImmKind::U20) {
      emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Auipc && di.imm_kind == sbt::ImmKind::U20) {
      const uint32_t val = static_cast<uint32_t>(di.pc + static_cast<uint32_t>(di.imm));
      emit_line(scalar_prefix() + "mov.u32 " + r(14) + ", " + hex_u32(val) + ";");
      emit_st_x_u32_scalar(di.rd, r(14), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "shl.b32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if ((di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical || di.emit.scalar_int_kind == ScalarIntKind::ShiftRightArithmetic) &&
        di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      if (di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical) {
        emit_line(scalar_prefix() + "shr.u32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      }
      else emit_line(scalar_prefix() + "shr.s32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 31;");
      emit_line(scalar_prefix() + "shl.b32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if ((di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical || di.emit.scalar_int_kind == ScalarIntKind::ShiftRightArithmetic) &&
        di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "and.b32 " + r(17) + ", " + r(15) + ", 31;");
      if (di.emit.scalar_int_kind == ScalarIntKind::ShiftRightLogical) emit_line(scalar_prefix() + "shr.u32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
      else emit_line(scalar_prefix() + "shr.s32 " + r(16) + ", " + r(14) + ", " + r(17) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::Xor && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "xor.b32 " + r(15) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThan && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "setp.lt.s32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThanU && di.operand_form == sbt::OperandForm::XRdRs1Rs2) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_ld_x_u32_scalar(r(15), di.rs2, pc);
      emit_line(scalar_prefix() + "setp.lt.u32 " + p(1) + ", " + r(14) + ", " + r(15) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(16) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(16), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThan && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      emit_line(scalar_prefix() + "setp.lt.s32 " + p(1) + ", " + r(14) + ", " + std::to_string(di.imm) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(15) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }
    if (di.emit.scalar_int_kind == ScalarIntKind::SetLessThanU && di.operand_form == sbt::OperandForm::XRdRs1Imm && di.imm_kind == sbt::ImmKind::I12) {
      emit_ld_x_u32_scalar(r(14), di.rs1, pc);
      const uint32_t imm_u = static_cast<uint32_t>(di.imm);
      emit_line(scalar_prefix() + "setp.lt.u32 " + p(1) + ", " + r(14) + ", " + hex_u32(imm_u) + ";");
      emit_line(scalar_prefix() + "selp.u32 " + r(15) + ", 1, 0, " + p(1) + ";");
      emit_st_x_u32_scalar(di.rd, r(15), pc);
      return;
    }

    // Vector loads/stores.
    // NOTE: `vlw.v` / `vsw.v` are Ventus GPU-private-memory indexed ops (Spike: insns/vlw_v.h, vsw_v.h).
    if (is_vector_memory(di, MemAccessKind::Load, MemoryAddrKind::Pds) && di.imm_kind == sbt::ImmKind::I12) {
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
    if (is_vector_memory(di, MemAccessKind::Store, MemoryAddrKind::Pds) && di.imm_kind == sbt::ImmKind::S12) {
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

    if (is_vector_memory(di, MemAccessKind::Load, MemoryAddrKind::Ordinary) && di.imm_kind == sbt::ImmKind::I12) {
      emit_line("add.u32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";"); // addr32
      if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
        emit_addr_map_and_ld_u8_zext_u32(r(15), r(14), pc);
        emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 24;");
        emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 24;");
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else if (di.emit.mem_width == MemWidth::Byte && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
        emit_addr_map_and_ld_u8_zext_u32(r(15), r(14), pc);
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::SignExtend) {
        emit_addr_map_and_ld_u16_zext_u32(r(15), r(14), pc);
        emit_line("shl.b32 " + r(15) + ", " + r(15) + ", 16;");
        emit_line("shr.s32 " + r(15) + ", " + r(15) + ", 16;");
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else if (di.emit.mem_width == MemWidth::Half && di.emit.mem_ext_kind == MemExtKind::ZeroExtend) {
        emit_addr_map_and_ld_u16_zext_u32(r(15), r(14), pc);
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      } else {
        emit_addr_map_and_ld_u32(r(15), r(14), pc);
        emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      }
      return;
    }

    if (is_vector_memory(di, MemAccessKind::Store, MemoryAddrKind::Ordinary) && di.imm_kind == sbt::ImmKind::S12) {
      emit_line("add.u32 " + r(14) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";"); // addr32
      if (di.emit.mem_width == MemWidth::Byte) {
        emit_line("cvt.u8.u32 " + u8(1) + ", " + v(di.rs2) + ";");
        emit_addr_map_and_st_u8(r(14), u8(1), pc);
      } else if (di.emit.mem_width == MemWidth::Half) {
        emit_line("cvt.u16.u32 " + u16(1) + ", " + v(di.rs2) + ";");
        emit_addr_map_and_st_u16(r(14), u16(1), pc);
      } else {
        emit_addr_map_and_st_u32(r(14), v(di.rs2), pc);
      }
      return;
    }

    // Vector register ops.
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::BroadcastScalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::InsertScalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::BroadcastImmediate) {
      emit_line("mov.u32 " + v(di.rd) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::MoveVector) {
      emit_line("mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::ExtractScalar) {
      require_scalar_exec_kind(di, ScalarExecKind::FixedLaneSensitive, pc);
      emit_trap_if_lane_inactive(/*lane=*/0u);
      emit_line("setp.eq.u32 " + p(2) + ", " + r(0) + ", 0;");
      emit_line("@" + p(2) + " mov.u32 " + x(di.rd) + ", " + v(di.rs2) + ";");
      emit_read_activemask(r(1));
      emit_line("shfl.sync.idx.b32 " + x(di.rd) + ", " + x(di.rd) + ", 0, 0x1f, " + r(1) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::LaneId) {
      emit_line("mov.u32 " + v(di.rd) + ", " + r(0) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
        di.emit.merge_kind == MergeKind::Vvm) {
      // Use v0 as a per-lane boolean mask (non-zero => true).
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
        di.emit.merge_kind == MergeKind::Vxm) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
        di.emit.merge_kind == MergeKind::Vim) {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + std::to_string(di.imm) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::VectorRegister && di.emit.vector_register_kind == VectorRegisterKind::Merge &&
        di.emit.merge_kind == MergeKind::Vfm) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.ne.u32 " + p(1) + ", " + v(0) + ", 0;");
      emit_line("selp.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + p(1) + ";");
      return;
    }

    if (di.mma.valid || (di.custom.valid && di.custom.family == sbt::CustomFamily::Mma)) {
      emit_mma_inst(di);
      return;
    }

    auto unpack_u32_to_halves = [&](const std::string &src_u32, const std::string &lo_h16, const std::string &hi_h16) {
      emit_line("mov.b32 {" + lo_h16 + ", " + hi_h16 + "}, " + src_u32 + ";");
    };
    auto pack_halves_to_u32 = [&](const std::string &dst_u32, const std::string &lo_h16, const std::string &hi_h16) {
      emit_line("mov.b32 " + dst_u32 + ", {" + lo_h16 + ", " + hi_h16 + "};");
    };
    auto emit_tanh_f32 = [&](const std::string &dst_f, const std::string &src_f) {
      const std::string tmp0 = tmp_f32();
      const std::string tmp1 = tmp_f32();
      emit_line("mul.rn.f32 " + tmp0 + ", " + src_f + ", -2.8853900817779268;");
      emit_line("ex2.approx.f32 " + tmp0 + ", " + tmp0 + ";");
      emit_line("add.rn.f32 " + tmp1 + ", " + tmp0 + ", 1.0;");
      emit_line("rcp.approx.f32 " + tmp1 + ", " + tmp1 + ";");
      emit_line("mul.rn.f32 " + tmp1 + ", " + tmp1 + ", 2.0;");
      emit_line("add.rn.f32 " + dst_f + ", " + tmp1 + ", -1.0;");
    };
    auto emit_silu_f32 = [&](const std::string &dst_f, const std::string &src_f) {
      const std::string tmp0 = tmp_f32();
      const std::string tmp1 = tmp_f32();
      emit_line("mul.rn.f32 " + tmp0 + ", " + src_f + ", -1.4426950408889634;");
      emit_line("ex2.approx.f32 " + tmp0 + ", " + tmp0 + ";");
      emit_line("add.rn.f32 " + tmp1 + ", " + tmp0 + ", 1.0;");
      emit_line("rcp.approx.f32 " + tmp1 + ", " + tmp1 + ";");
      emit_line("mul.rn.f32 " + dst_f + ", " + src_f + ", " + tmp1 + ";");
    };
    auto emit_gelu_f32 = [&](const std::string &dst_f, const std::string &src_f) {
      const std::string poly = tmp_f32();
      const std::string tanh_v = tmp_f32();
      emit_line("mul.rn.f32 " + poly + ", " + src_f + ", " + src_f + ";");
      emit_line("mul.rn.f32 " + poly + ", " + poly + ", " + src_f + ";");
      emit_line("mad.rn.f32 " + poly + ", " + poly + ", 0.044715, " + src_f + ";");
      emit_line("mul.rn.f32 " + poly + ", " + poly + ", 0.7978845608028654;");
      emit_tanh_f32(tanh_v, poly);
      emit_line("add.rn.f32 " + tanh_v + ", " + tanh_v + ", 1.0;");
      emit_line("mul.rn.f32 " + tanh_v + ", " + tanh_v + ", 0.5;");
      emit_line("mul.rn.f32 " + dst_f + ", " + src_f + ", " + tanh_v + ";");
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
    auto emit_custom_rsqrt_dual_lane = [&](bool bf16_kind, const std::string &src_h16, const std::string &src_f32,
                                           const std::string &dst_f32, const std::string &dst_h16) {
      const std::string exp_mask = bf16_kind ? "0x7f80" : "0x7c00";
      const std::string frac_mask = bf16_kind ? "0x007f" : "0x03ff";
      const std::string pos_inf = bf16_kind ? "0x7f80" : "0x7c00";
      const std::string convert_to_f32 = bf16_kind ? "cvt.f32.bf16 " : "cvt.f32.f16 ";
      const std::string convert_from_f32 = bf16_kind ? "cvt.rn.bf16.f32 " : "cvt.rn.f16.f32 ";
      const std::string src_u16 = tmp_u16();
      const std::string src_u32 = tmp_b32();
      const std::string sign_bits = tmp_b32();
      const std::string exp_bits = tmp_b32();
      const std::string frac_bits = tmp_b32();
      const std::string is_sign = tmp_pred();
      const std::string is_exp_zero = tmp_pred();
      const std::string is_exp_all1 = tmp_pred();
      const std::string is_frac_zero = tmp_pred();
      const std::string is_zero = tmp_pred();
      const std::string is_inf = tmp_pred();
      const std::string is_nan = tmp_pred();
      const std::string is_neg_nonzero = tmp_pred();
      const std::string is_pos_inf = tmp_pred();
      emit_line("mov.b16 " + src_u16 + ", " + src_h16 + ";");
      emit_line("cvt.u32.u16 " + src_u32 + ", " + src_u16 + ";");
      emit_line("and.b32 " + sign_bits + ", " + src_u32 + ", 0x8000;");
      emit_line("and.b32 " + exp_bits + ", " + src_u32 + ", " + exp_mask + ";");
      emit_line("and.b32 " + frac_bits + ", " + src_u32 + ", " + frac_mask + ";");
      emit_line("setp.ne.u32 " + is_sign + ", " + sign_bits + ", 0;");
      emit_line("setp.eq.u32 " + is_exp_zero + ", " + exp_bits + ", 0;");
      emit_line("setp.eq.u32 " + is_exp_all1 + ", " + exp_bits + ", " + exp_mask + ";");
      emit_line("setp.eq.u32 " + is_frac_zero + ", " + frac_bits + ", 0;");
      emit_line("and.pred " + is_zero + ", " + is_exp_zero + ", " + is_frac_zero + ";");
      emit_line("and.pred " + is_inf + ", " + is_exp_all1 + ", " + is_frac_zero + ";");
      emit_line("not.pred " + is_nan + ", " + is_frac_zero + ";");
      emit_line("and.pred " + is_nan + ", " + is_exp_all1 + ", " + is_nan + ";");
      emit_line("not.pred " + is_neg_nonzero + ", " + is_zero + ";");
      emit_line("and.pred " + is_neg_nonzero + ", " + is_sign + ", " + is_neg_nonzero + ";");
      emit_line("not.pred " + is_pos_inf + ", " + is_sign + ";");
      emit_line("and.pred " + is_pos_inf + ", " + is_inf + ", " + is_pos_inf + ";");
      emit_line(convert_to_f32 + src_f32 + ", " + src_h16 + ";");
      emit_custom_sfu_f32(dst_f32, src_f32, sbt::CustomSubOp::Rsqrt);
      emit_line(convert_from_f32 + dst_h16 + ", " + dst_f32 + ";");
      emit_line("@" + is_zero + " mov.b16 " + dst_h16 + ", " + pos_inf + ";");
      emit_line("@" + is_neg_nonzero + " mov.b16 " + dst_h16 + ", 0x7fff;");
      emit_line("@" + is_pos_inf + " mov.b16 " + dst_h16 + ", 0;");
      emit_line("@" + is_nan + " mov.b16 " + dst_h16 + ", 0x7fff;");
    };
    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Shuffle) {
      require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", func_name, pc, di.name));
      emit_read_activemask(r(1));
      const std::string shuffle_arg = tmp_b32();
      emit_line("mov.u32 " + shuffle_arg + ", " + std::to_string(di.imm & 31) + ";");
      if (di.custom.subop == sbt::CustomSubOp::ShuffleIdx) {
        emit_line("shfl.sync.idx.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x1f, " + r(1) + ";");
      } else if (di.custom.subop == sbt::CustomSubOp::ShuffleUp) {
        emit_line("shfl.sync.up.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x0, " + r(1) + ";");
      } else if (di.custom.subop == sbt::CustomSubOp::ShuffleDown) {
        emit_line("shfl.sync.down.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x1f, " + r(1) + ";");
      } else if (di.custom.subop == sbt::CustomSubOp::ShuffleBfly) {
        emit_line("shfl.sync.bfly.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + shuffle_arg + ", 0x1f, " + r(1) + ";");
      } else {
        throw EmitError("invalid.custom.payload", func_name, pc, di.name);
      }
      return;
    }

    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Convert &&
        di.custom.subop == sbt::CustomSubOp::CvtF32FromF16 && di.custom.dtype == sbt::CustomDataType::Fp16) {
      const std::string lo_half = tmp_b16();
      const std::string hi_half = tmp_b16();
      const std::string dst = tmp_f32();
      unpack_u32_to_halves(v(di.rs2), lo_half, hi_half);
      emit_line("cvt.f32.f16 " + dst + ", " + lo_half + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + dst + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Convert &&
        di.custom.subop == sbt::CustomSubOp::CvtF16FromF32 && di.custom.dtype == sbt::CustomDataType::Fp16) {
      const std::string src = tmp_f32();
      const std::string lo_half = tmp_b16();
      const std::string hi_half = tmp_b16();
      const std::string packed = tmp_b32();
      emit_line("mov.b32 " + src + ", " + v(di.rs2) + ";");
      emit_line("cvt.rn.f16.f32 " + lo_half + ", " + src + ";");
      emit_line("mov.b16 " + hi_half + ", 0;");
      pack_halves_to_u32(packed, lo_half, hi_half);
      emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Convert &&
        di.custom.subop == sbt::CustomSubOp::CvtF32FromBf16 && di.custom.dtype == sbt::CustomDataType::Bf16) {
      const std::string lo_half = tmp_b16();
      const std::string hi_half = tmp_b16();
      const std::string dst = tmp_f32();
      unpack_u32_to_halves(v(di.rs2), lo_half, hi_half);
      emit_line("cvt.f32.bf16 " + dst + ", " + lo_half + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + dst + ";");
      return;
    }
    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Convert &&
        di.custom.subop == sbt::CustomSubOp::CvtBf16FromF32 && di.custom.dtype == sbt::CustomDataType::Bf16) {
      const std::string src = tmp_f32();
      const std::string lo_half = tmp_b16();
      const std::string hi_half = tmp_b16();
      const std::string packed = tmp_b32();
      emit_line("mov.b32 " + src + ", " + v(di.rs2) + ";");
      emit_line("cvt.rn.bf16.f32 " + lo_half + ", " + src + ";");
      emit_line("mov.b16 " + hi_half + ", 0;");
      pack_halves_to_u32(packed, lo_half, hi_half);
      emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return;
    }

    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::PackedArith &&
        di.custom.dtype == sbt::CustomDataType::F16x2) {
      const std::string packed = tmp_b32();
      if (di.custom.subop == sbt::CustomSubOp::Add) emit_line("add.rn.f16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      else if (di.custom.subop == sbt::CustomSubOp::Mul) emit_line("mul.rn.f16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      else if (di.custom.subop == sbt::CustomSubOp::Fma) emit_line("fma.rn.f16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      else throw EmitError("invalid.custom.payload", func_name, pc, di.name);
      emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return;
    }

    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::PackedArith &&
        di.custom.dtype == sbt::CustomDataType::Bf16x2) {
      if (di.custom.subop == sbt::CustomSubOp::Fma) {
        const std::string packed = tmp_b32();
        emit_line("fma.rn.bf16x2 " + packed + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
        emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
        return;
      }
      const std::string a0 = tmp_b16();
      const std::string a1 = tmp_b16();
      const std::string b0 = tmp_b16();
      const std::string b1 = tmp_b16();
      const std::string fa0 = tmp_f32();
      const std::string fa1 = tmp_f32();
      const std::string fb0 = tmp_f32();
      const std::string fb1 = tmp_f32();
      const std::string out0 = tmp_f32();
      const std::string out1 = tmp_f32();
      const std::string packed0 = tmp_b16();
      const std::string packed1 = tmp_b16();
      const std::string packed = tmp_b32();
      unpack_u32_to_halves(v(di.rs1), a0, a1);
      unpack_u32_to_halves(v(di.rs2), b0, b1);
      emit_line("cvt.f32.bf16 " + fa0 + ", " + a0 + ";");
      emit_line("cvt.f32.bf16 " + fa1 + ", " + a1 + ";");
      emit_line("cvt.f32.bf16 " + fb0 + ", " + b0 + ";");
      emit_line("cvt.f32.bf16 " + fb1 + ", " + b1 + ";");
      if (di.custom.subop == sbt::CustomSubOp::Add) {
        emit_line("add.rn.f32 " + out0 + ", " + fa0 + ", " + fb0 + ";");
        emit_line("add.rn.f32 " + out1 + ", " + fa1 + ", " + fb1 + ";");
      } else if (di.custom.subop == sbt::CustomSubOp::Mul) {
        emit_line("mul.rn.f32 " + out0 + ", " + fa0 + ", " + fb0 + ";");
        emit_line("mul.rn.f32 " + out1 + ", " + fa1 + ", " + fb1 + ";");
      } else {
        throw EmitError("invalid.custom.payload", func_name, pc, di.name);
      }
      emit_line("cvt.rn.bf16.f32 " + packed0 + ", " + out0 + ";");
      emit_line("cvt.rn.bf16.f32 " + packed1 + ", " + out1 + ";");
      pack_halves_to_u32(packed, packed0, packed1);
      emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return;
    }

    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Sfu &&
        di.custom.dtype == sbt::CustomDataType::Fp32) {
      require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", func_name, pc, di.name));
      const std::string src = tmp_f32();
      const std::string dst = tmp_f32();
      emit_line("mov.b32 " + src + ", " + v(di.rs2) + ";");
      emit_custom_sfu_f32(dst, src, di.custom.subop);
      emit_line("mov.b32 " + v(di.rd) + ", " + dst + ";");
      return;
    }

    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Sfu &&
        di.custom.dtype == sbt::CustomDataType::F16x2) {
      require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", func_name, pc, di.name));
      const std::string in0 = tmp_b16();
      const std::string in1 = tmp_b16();
      const std::string out0 = tmp_b16();
      const std::string out1 = tmp_b16();
      const std::string f0v = tmp_f32();
      const std::string f1v = tmp_f32();
      const std::string f2v = tmp_f32();
      const std::string f3v = tmp_f32();
      const std::string packed = tmp_b32();
      unpack_u32_to_halves(v(di.rs2), in0, in1);
      if (di.custom.subop == sbt::CustomSubOp::Rsqrt) {
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/false, in0, f0v, f2v, out0);
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/false, in1, f1v, f3v, out1);
      } else {
        emit_line("cvt.f32.f16 " + f0v + ", " + in0 + ";");
        emit_line("cvt.f32.f16 " + f1v + ", " + in1 + ";");
        emit_custom_sfu_f32(f2v, f0v, di.custom.subop);
        emit_custom_sfu_f32(f3v, f1v, di.custom.subop);
        emit_line("cvt.rn.f16.f32 " + out0 + ", " + f2v + ";");
        emit_line("cvt.rn.f16.f32 " + out1 + ", " + f3v + ";");
      }
      pack_halves_to_u32(packed, out0, out1);
      emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return;
    }

    if (di.emit.domain == EmitDomain::Custom && di.custom.valid && di.custom.family == sbt::CustomFamily::Sfu &&
        di.custom.dtype == sbt::CustomDataType::Bf16x2) {
      require(di.custom.subop != sbt::CustomSubOp::None, EmitError("invalid.custom.payload", func_name, pc, di.name));
      const std::string in0 = tmp_b16();
      const std::string in1 = tmp_b16();
      const std::string out0 = tmp_b16();
      const std::string out1 = tmp_b16();
      const std::string f0v = tmp_f32();
      const std::string f1v = tmp_f32();
      const std::string f2v = tmp_f32();
      const std::string f3v = tmp_f32();
      const std::string packed = tmp_b32();
      unpack_u32_to_halves(v(di.rs2), in0, in1);
      if (di.custom.subop == sbt::CustomSubOp::Rsqrt) {
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/true, in0, f0v, f2v, out0);
        emit_custom_rsqrt_dual_lane(/*bf16_kind=*/true, in1, f1v, f3v, out1);
      } else {
        emit_line("cvt.f32.bf16 " + f0v + ", " + in0 + ";");
        emit_line("cvt.f32.bf16 " + f1v + ", " + in1 + ";");
        emit_custom_sfu_f32(f2v, f0v, di.custom.subop);
        emit_custom_sfu_f32(f3v, f1v, di.custom.subop);
        emit_line("cvt.rn.bf16.f32 " + out0 + ", " + f2v + ";");
        emit_line("cvt.rn.bf16.f32 " + out1 + ", " + f3v + ";");
      }
      pack_halves_to_u32(packed, out0, out1);
      emit_line("mov.u32 " + v(di.rd) + ", " + packed + ";");
      return;
    }

    if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("add.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("add.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("add.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Add && di.operand_form == sbt::OperandForm::VRdRs1VectorImm && di.imm_kind == sbt::ImmKind::I12) {
      emit_line("add.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::RSub && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("sub.s32 " + v(di.rd) + ", " + std::to_string(di.imm) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::RSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("sub.u32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MinU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("min.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MinU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("min.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("min.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("min.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MaxU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("max.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MaxU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("max.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("max.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("max.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Sub && di.operand_form == sbt::OperandForm::VRdRs1VectorImm && di.imm_kind == sbt::ImmKind::I12) {
      emit_line("sub.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::And && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::And && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::And && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("and.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Or && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Or && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Or && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("or.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Xor && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Xor && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Xor && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("xor.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
      emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftLeft && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
      emit_line("shl.b32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftRightLogical && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftRightLogical && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
      emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftRightLogical && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
      emit_line("shr.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftRightArithmetic && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftRightArithmetic && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("and.b32 " + r(14) + ", " + v(di.rs1) + ", 31;");
      emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::ShiftRightArithmetic && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("and.b32 " + r(14) + ", " + r(14) + ", 31;");
      emit_line("shr.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.lo.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("mul.lo.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MulH && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      // Spike semantics (vmulh vd,vs2,rs1): high half of signed multiplication.
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.hi.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MulH && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("mul.hi.s32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MulHU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.hi.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MulHU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("mul.hi.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MulHSU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("cvt.s64.s32 " + rd(16) + ", " + v(di.rs2) + ";");
      emit_line("cvt.s64.u32 " + rd(17) + ", " + r(14) + ";");
      emit_line("mul.lo.s64 " + rd(18) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
      emit_line("cvt.u32.s64 " + r(15) + ", " + rd(18) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MulHSU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("cvt.s64.s32 " + rd(16) + ", " + v(di.rs2) + ";");
      emit_line("cvt.s64.u32 " + rd(17) + ", " + v(di.rs1) + ";");
      emit_line("mul.lo.s64 " + rd(18) + ", " + rd(16) + ", " + rd(17) + ";");
      emit_line("shr.s64 " + rd(18) + ", " + rd(18) + ", 32;");
      emit_line("cvt.u32.s64 " + r(15) + ", " + rd(18) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(15) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::DivU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
      emit_line("@!" + p(1) + " div.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::DivU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", 0xffffffff;");
      emit_line("@!" + p(1) + " div.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::RemU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + r(14) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      emit_line("@!" + p(1) + " rem.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::RemU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs1) + ", 0;");
      emit_line("@" + p(1) + " mov.u32 " + v(di.rd) + ", " + v(di.rs2) + ";");
      emit_line("@!" + p(1) + " rem.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
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
    if (di.emit.vector_int_kind == VectorIntKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
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
    if (di.emit.vector_int_kind == VectorIntKind::Rem && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
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
    if (di.emit.vector_int_kind == VectorIntKind::Rem && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
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
    if (di.emit.vector_int_kind == VectorIntKind::Madd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // RISC-V V: vmadd vd,vs1,vs2 => vd = (vd * vs1) + vs2
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::Madd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      // RISC-V V: vmadd vd,rs1,vs2 => vd = (vd * rs1) + vs2
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // RISC-V V: vnmsub vd,vs2,vs1 (vd is also the accumulator)
      // Spike: vd = -(vd * vs1) + vs2.
      emit_line("mul.lo.u32 " + r(14) + ", " + v(di.rd) + ", " + v(di.rs1) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.lo.u32 " + r(15) + ", " + v(di.rd) + ", " + r(14) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rs2) + ", " + r(15) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // Spike: vd = (vs1 * vs2) + vd
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + v(di.rs1) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mad.lo.s32 " + v(di.rd) + ", " + r(14) + ", " + v(di.rs2) + ", " + v(di.rd) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("mul.lo.s32 " + r(14) + ", " + v(di.rs1) + ", " + v(di.rs2) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_int_kind == VectorIntKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mul.lo.s32 " + r(15) + ", " + r(14) + ", " + v(di.rs2) + ";");
      emit_line("sub.u32 " + v(di.rd) + ", " + v(di.rd) + ", " + r(15) + ";");
      return;
    }

    // Vector compare -> 0/1 mask (treat mask registers as u32).
    auto emit_mask_from_pred = [&](const std::string &pred) { emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + pred + ";"); };

    if (di.emit.vector_compare_kind == VectorCompareKind::Eq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Eq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Eq && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("setp.eq.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Ne && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Ne && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Ne && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::LtU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.lt.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Lt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.lt.s32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::LeU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::LeU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::LeU && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("setp.le.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Le && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + v(di.rs1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Le && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::GtU && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("setp.gt.u32 " + p(1) + ", " + v(di.rs2) + ", " + hex_u32(static_cast<uint32_t>(di.imm)) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::GtU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.gt.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Gt && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("setp.gt.s32 " + p(1) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Gt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.gt.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_mask_from_pred(p(1));
      return;
    }

    // Mask boolean ops (treat non-zero as true, produce 0/1).
    if (di.emit.domain == EmitDomain::VectorMask) {
      emit_line("setp.ne.u32 " + p(1) + ", " + v(di.rs2) + ", 0;");
      emit_line("setp.ne.u32 " + p(2) + ", " + v(di.rs1) + ", 0;");

      const bool invert_b = (di.emit.vector_mask_kind == VectorMaskKind::AndNot || di.emit.vector_mask_kind == VectorMaskKind::OrNot);
      if (invert_b) emit_line("not.pred " + p(2) + ", " + p(2) + ";");

      if (di.emit.vector_mask_kind == VectorMaskKind::And || di.emit.vector_mask_kind == VectorMaskKind::AndNot) {
        emit_line("and.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
      } else if (di.emit.vector_mask_kind == VectorMaskKind::Or || di.emit.vector_mask_kind == VectorMaskKind::OrNot) {
        emit_line("or.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
      } else {
        emit_line("xor.pred " + p(3) + ", " + p(1) + ", " + p(2) + ";");
      }

      if (di.emit.vector_mask_kind == VectorMaskKind::XNor || di.emit.vector_mask_kind == VectorMaskKind::Nand ||
          di.emit.vector_mask_kind == VectorMaskKind::Nor) {
        emit_line("not.pred " + p(3) + ", " + p(3) + ";");
      }
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
    if (di.emit.vector_compare_kind == VectorCompareKind::FEq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_vf_cmp_vv("setp.eq.f32");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FEq && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_vf_cmp_vf("setp.eq.f32");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FLe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_vf_cmp_vv("setp.le.f32");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FLe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_vf_cmp_vf("setp.le.f32");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FLt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_vf_cmp_vv("setp.lt.f32");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FLt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_vf_cmp_vf("setp.lt.f32");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FGt) {
      // Spike: res = (rs1 < vs2)
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("setp.lt.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FGe) {
      // Spike: res = (rs1 <= vs2)
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("setp.le.f32 " + p(1) + ", " + f(0) + ", " + f(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FNe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      emit_vf_cmp_vv("setp.eq.f32");
      emit_line("not.pred " + p(1) + ", " + p(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::FNe && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_vf_cmp_vf("setp.eq.f32");
      emit_line("not.pred " + p(1) + ", " + p(1) + ";");
      emit_mask_from_pred(p(1));
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Lt && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.lt.s32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      // Spike semantics produce 0/1 (not 0xffffffff/0). Many kernels use `vxor.vi 1` to invert.
      emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::LtU && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("setp.lt.u32 " + p(1) + ", " + v(di.rs2) + ", " + r(14) + ";");
      emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
      return;
    }
    if (di.emit.vector_compare_kind == VectorCompareKind::Le && di.operand_form == sbt::OperandForm::VRdRs2VectorImm) {
      emit_line("setp.le.s32 " + p(1) + ", " + v(di.rs2) + ", " + std::to_string(di.imm) + ";");
      emit_line("selp.u32 " + v(di.rd) + ", 1, 0, " + p(1) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::FloatFromInt) {
      emit_line("cvt.rn.f32.s32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::FloatFromUInt) {
      emit_line("cvt.rn.f32.u32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(0) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::IntFromFloat) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rni.s32.f32 " + r(14) + ", " + f(0) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::UIntFromFloat) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rni.u32.f32 " + r(14) + ", " + f(0) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::IntFromFloatRtz) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rzi.s32.f32 " + r(14) + ", " + f(0) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::UIntFromFloatRtz) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("cvt.rzi.u32.f32 " + r(14) + ", " + f(0) + ";");
      emit_line("mov.u32 " + v(di.rd) + ", " + r(14) + ";");
      return;
    }
    if (di.emit.vector_convert_kind == VectorConvertKind::Classify) {
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
    if (di.emit.vector_fp_kind == VectorFpKind::Exp) {
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

    if (di.emit.vector_fp_kind == VectorFpKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("add.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Add && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("add.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("sub.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Sub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("sub.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("mul.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Mul && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("mul.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("div.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Div && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("div.rn.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::RSub) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("sub.rn.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::RDiv) {
      emit_ld_x_u32_all(r(14), di.rs1, pc);
      emit_line("mov.b32 " + f(0) + ", " + r(14) + ";");
      emit_line("mov.b32 " + f(1) + ", " + v(di.rs2) + ";");
      emit_line("div.rn.f32 " + f(2) + ", " + f(0) + ", " + f(1) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(2) + ";");
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("min.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Min && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("min.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) { vf_binop("max.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::Max && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) { vf_binop_vf("max.f32"); return; }
    if (di.emit.vector_fp_kind == VectorFpKind::MAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
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
    if (di.emit.vector_fp_kind == VectorFpKind::MAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), false, r(15), false, v(di.rs2), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::MSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      vf_fma_bits(v(di.rd), false, v(di.rs1), false, v(di.rs2), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::MSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), false, r(15), false, v(di.rs2), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      vf_fma_bits(v(di.rd), true, v(di.rs1), false, v(di.rs2), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMAdd && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), true, r(15), false, v(di.rs2), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      vf_fma_bits(v(di.rd), true, v(di.rs1), false, v(di.rs2), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMSub && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(v(di.rd), true, r(15), false, v(di.rs2), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // Spike: vd = (vs1 * vs2) + vd
      vf_fma_bits(v(di.rs1), false, v(di.rs2), false, v(di.rd), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::MAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), false, v(di.rd), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // Spike: vd = -(vs1 * vs2) - vd  == fma(-vs2, vs1, -vd)
      vf_fma_bits(v(di.rs2), true, v(di.rs1), false, v(di.rd), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMAcc && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      // Spike: vd = -(rs1 * vs2) - vd == fma(rs1, -vs2, -vd)
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), true, v(di.rd), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::MSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // Spike: vd = (vs1 * vs2) - vd == fma(vs1, vs2, -vd)
      vf_fma_bits(v(di.rs1), false, v(di.rs2), false, v(di.rd), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::MSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), false, v(di.rd), true);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Vector) {
      // Spike: vd = -(vs2 * vs1) + vd == fma(-vs1, vs2, vd)
      vf_fma_bits(v(di.rs1), true, v(di.rs2), false, v(di.rd), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::NMSac && di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
      // Spike: vd = -(rs1 * vs2) + vd == fma(rs1, -vs2, vd)
      emit_ld_x_u32_all(r(15), di.rs1, pc);
      vf_fma_bits(r(15), false, v(di.rs2), true, v(di.rd), false);
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::Sqrt) {
      emit_line("mov.b32 " + f(0) + ", " + v(di.rs2) + ";");
      emit_line("sqrt.rn.f32 " + f(1) + ", " + f(0) + ";");
      emit_line("mov.b32 " + v(di.rd) + ", " + f(1) + ";");
      return;
    }
    if (di.emit.vector_fp_kind == VectorFpKind::SignInject) {
      if (di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) {
        emit_ld_x_u32_all(r(14), di.rs1, pc);
      }
      const std::string sign_src = (di.operand_form == sbt::OperandForm::VRdRs2VectorRs1Scalar) ? r(14) : v(di.rs1);
      emit_line("and.b32 " + r(15) + ", " + v(di.rs2) + ", 0x7fffffff;"); // magnitude from vs2
      if (di.emit.vector_fp_sign_inject_kind == VectorFpSignInjectKind::CopySign) {
        emit_line("and.b32 " + r(16) + ", " + sign_src + ", 0x80000000;");
      } else if (di.emit.vector_fp_sign_inject_kind == VectorFpSignInjectKind::NegateSign) {
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
    // Load params and compute global/shared base pointers.
    emit_line("ld.param.u64 " + rd(10) + ", [global_base];");
    emit_line("cvta.to.global.u64 " + rd(0) + ", " + rd(10) + ";");
    emit_line("ld.param.u32 " + r(30) + ", [knl_vaddr];");
    emit_line("ld.param.u32 " + r(28) + ", [pds_base_vaddr];");
    emit_line("ld.param.u32 " + r(29) + ", [pds_size_per_thread];");
    emit_line("ld.param.u32 " + r(26) + ", [pds_bitmap_base_vaddr];");
    emit_line("ld.param.u32 " + r(27) + ", [pds_pool_num_blocks];");

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
    emit_line("shl.b32 " + r(13) + ", " + r(10) + ", 10;");
    emit_line("cvt.u64.u32 " + rd(13) + ", " + r(13) + ";");
    emit_line("add.u64 " + rd(3) + ", " + rd(2) + ", " + rd(13) + ";");

    // lds_ptr = shmem_base + warps_per_block * 1024
    emit_line("shl.b32 " + r(14) + ", " + r(12) + ", 10;");
    emit_line("cvt.u64.u32 " + rd(14) + ", " + r(14) + ";");
    emit_line("add.u64 " + rd(4) + ", " + rd(2) + ", " + rd(14) + ";");

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

    // x2 = shared_base + warp_id * stack_stride (default: 1024 bytes => <<10)
    emit_line("shl.b32 " + r(15) + ", " + r(10) + ", 10;");
    emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_st_x_u32_scalar(/*x2=*/2, r(15), /*pc_for_err=*/cfg.start);

    // Match `_start` ABI: s0 (x8) points to the base of the kernel LDS region:
    //   s0 = CSR_LDS + CSR_NUMW*1024
    // In this backend `shared_base_vaddr` models the CSR_LDS numeric base, and `warps_per_block` models CSR_NUMW.
    // Note: kernels may further adjust s0 in their own prologue (e.g. `addi s0, s0, <frame_bytes>`). We treat that
    // as frame allocation and do not attempt to compensate it here.
    emit_line("shl.b32 " + r(15) + ", " + r(12) + ", 10;");
    emit_line("add.u32 " + r(15) + ", " + r(15) + ", " + hex_u32(opt.shared_base_vaddr) + ";");
    emit_st_x_u32_scalar(/*x8=*/8, r(15), /*pc_for_err=*/cfg.start);

    // x10 (a0) is the first argument register. PoCL Ventus kernels expect:
    //   a0 = *(u32*)(CSR_KNL + 4)  (arg buffer base)
    // because the original `_start` loads it from the hardware metadata buffer before jumping to the kernel entry.
    emit_line("add.u32 " + r(16) + ", " + r(30) + ", 4;"); // arg_base field address
    emit_line("add.u32 " + r(16) + ", " + r(16) + ", 0;"); // keep in u32 reg
    emit_addr_map_and_ld_u32_scalar(r(17), r(16), /*pc_for_err=*/cfg.start);
    emit_st_x_u32_scalar(/*x10=*/10, r(17), /*pc_for_err=*/cfg.start);

    emit_warp_sync();

    // Fallthrough to entry BB.
  }

  void emit_func_prologue() {
    // Pass-through params computed in the caller and required for address mapping / CSR reads.
    emit_line("mov.u32 " + r(0) + ", %laneid;");
    emit_load_runtime_env_blob("__sbt_runtime_env_in");
    emit_load_machine_ctx_blob("__sbt_machine_ctx_in");
    emit_line("mov.u64 " + rd(2) + ", __sbt_shmem;");
    emit_line("shl.b32 " + r(14) + ", " + r(12) + ", 10;");
    emit_line("cvt.u64.u32 " + rd(14) + ", " + r(14) + ";");
    emit_line("add.u64 " + rd(4) + ", " + rd(2) + ", " + rd(14) + ";");
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
    if (is_ret(last) || (last.emit.domain == EmitDomain::StructuredControl &&
                         last.emit.structured_control_kind == StructuredControlKind::EndPrg)) {
      return;
    }
    if (is_scalar_branch(last) || is_vector_branch(last)) return;
    emit_line("bra " + target_label_for_edge(bb.start, *dst) + ";");
  }

  void emit_body() {
    prepare_control_protocol();
    if (is_entry) emit_prologue();
    else emit_func_prologue();

    // Emit blocks in address order.
    for (const auto &bb : cfg.blocks) {
      emit_boundary_labels_for_block(bb.start);
      emit_label(label_bb(bb.start));
      for (size_t idx : bb.inst_indices) {
        emit_one_inst(cfg.insts[idx]);
      }
      emit_fallthrough_edge_if_needed(bb);
    }

    if (is_entry) emit_entry_header();
    else emit_func_header();
    emit_fixed_reg_decls();
    emit_virtual_temp_reg_decls();
    out << body.str();
    out << "}\n\n";
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
