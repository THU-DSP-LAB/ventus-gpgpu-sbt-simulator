#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

void emit_helper_func_signature(std::ostringstream &out, const std::string &ptx_name) {
  out << ".func (\n";
  out << "    .param .align 4 .b8 __sbt_mutable_state_out[" << kMutableStateBytes << "]\n";
  out << ") " << ptx_name << "(\n";
  out << "    .param .align 4 .b8 __sbt_mutable_state_in[" << kMutableStateBytes << "],\n";
  out << "    .param .align 4 .b8 __sbt_machine_ctx_in[" << kMachineCtxBytes << "],\n";
  out << "    .param .align 8 .b8 __sbt_runtime_env_in[" << kRuntimeEnvBytes << "]\n";
  out << ")";
}

void EmitCtx::emit_store_param_u32(const std::string &base, uint32_t offset, const std::string &src_r, const std::string &prefix) {
  emit_line(prefix + "st.param.u32 [" + base + "+" + std::to_string(offset) + "], " + src_r + ";");
}

void EmitCtx::emit_store_param_u64(const std::string &base, uint32_t offset, const std::string &src_rd) {
  emit_line("st.param.u64 [" + base + "+" + std::to_string(offset) + "], " + src_rd + ";");
}

void EmitCtx::emit_load_param_u32(const std::string &dst_r, const std::string &base, uint32_t offset, const std::string &prefix) {
  emit_line(prefix + "ld.param.u32 " + dst_r + ", [" + base + "+" + std::to_string(offset) + "];");
}

void EmitCtx::emit_load_param_u64(const std::string &dst_rd, const std::string &base, uint32_t offset) {
  emit_line("ld.param.u64 " + dst_rd + ", [" + base + "+" + std::to_string(offset) + "];");
}

void EmitCtx::emit_store_mutable_state_blob(const std::string &blob_name) {
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

void EmitCtx::emit_restore_mutable_state_blob(const std::string &blob_name) {
  emit_load_param_u32(r(2), blob_name, kMutableLeaderOffset);
  emit_refresh_leader_predicate();
  for (uint32_t i = 1; i < kNumXRegs; ++i) {
    emit_load_param_u32(x(static_cast<int>(i)), blob_name, kMutableXOffset + i * kBlobWordBytes);
  }
  for (uint32_t i = 0; i < kNumVRegs; ++i) {
    emit_load_param_u32(v(static_cast<int>(i)), blob_name, kMutableVOffset + i * kBlobWordBytes);
  }
}

void EmitCtx::emit_store_runtime_env_blob(const std::string &blob_name) {
  emit_store_param_u64(blob_name, kRuntimeGlobalOffset, rd(0));
}

void EmitCtx::emit_store_machine_ctx_blob(const std::string &blob_name) {
  emit_store_param_u32(blob_name, kMachineKnlOffset, r(30));
  emit_store_param_u32(blob_name, kMachinePdsBaseOffset, r(28));
  emit_store_param_u32(blob_name, kMachinePdsSizeOffset, r(29));
  emit_store_param_u32(blob_name, kMachineWarpIdOffset, r(10));
  emit_store_param_u32(blob_name, kMachineWarpsPerBlockOffset, r(12));
}

void EmitCtx::emit_load_runtime_env_blob(const std::string &blob_name) {
  emit_load_param_u64(rd(0), blob_name, kRuntimeGlobalOffset);
}

void EmitCtx::emit_load_machine_ctx_blob(const std::string &blob_name) {
  emit_load_param_u32(r(30), blob_name, kMachineKnlOffset);
  emit_load_param_u32(r(28), blob_name, kMachinePdsBaseOffset);
  emit_load_param_u32(r(29), blob_name, kMachinePdsSizeOffset);
  emit_load_param_u32(r(10), blob_name, kMachineWarpIdOffset);
  emit_load_param_u32(r(12), blob_name, kMachineWarpsPerBlockOffset);
}

void EmitCtx::emit_direct_call(const std::string &callee_ptx) {
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

} // namespace sbt::ptx::detail
