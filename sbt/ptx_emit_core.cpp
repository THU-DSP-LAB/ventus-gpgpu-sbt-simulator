#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

EmitCtx::EmitCtx(const sbt::cfg::FunctionCfg &cfg_, const std::unordered_map<uint32_t, std::string> &sym_by_addr_,
                 const std::string &func_name_, const std::string &ptx_name_, const Options &opt_, const ModuleInfo &mod_, bool is_entry_)
    : cfg(cfg_), sym_by_addr(sym_by_addr_), func_name(func_name_), ptx_name(ptx_name_), opt(opt_), mod(mod_), is_entry(is_entry_) {}

void EmitCtx::require_scalar_exec_kind(const sbt::DecodedInst &di, ScalarExecKind expected, uint32_t pc_for_err) const {
  require(scalar_exec_kind_for_inst(di, func_name) == expected,
          EmitError("invalid.scalar_exec", func_name, pc_for_err, std::string(di.name)));
}

void EmitCtx::require_uniform_pure_scalar(const sbt::DecodedInst &di, uint32_t pc_for_err) const {
  require_scalar_exec_kind(di, ScalarExecKind::UniformPure, pc_for_err);
}

void EmitCtx::emit_fixed_reg_decls() {
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

void EmitCtx::emit_virtual_temp_reg_decls() {
  for (size_t kind = 0; kind < kVirtualTempSpecs.size(); ++kind) {
    const auto &spec = kVirtualTempSpecs[kind];
    for (uint32_t idx = 0; idx < virtual_temp_counts[kind]; ++idx) {
      out << "  .reg " << spec.ptx_type << " " << spec.name_prefix << idx << ";\n";
    }
  }
}

void EmitCtx::emit_entry_header() {
  out << ".visible .entry " << ptx_name << "(\n";
  out << "    .param .u64 global_base,\n";
  out << "    .param .u32 knl_vaddr,\n";
  out << "    .param .u32 pds_base_vaddr,\n";
  out << "    .param .u32 pds_size_per_thread,\n";
  out << "    .param .u32 pds_bitmap_base_vaddr,\n";
  out << "    .param .u32 pds_pool_num_blocks\n";
  out << ")\n{\n";
}

void EmitCtx::emit_func_header() {
  emit_helper_func_signature(out, ptx_name);
  out << "\n{\n";
}

void EmitCtx::prepare_control_protocol() {
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

std::string EmitCtx::target_label_for_edge(uint32_t src_block, uint32_t dst_block) const {
  (void)src_block;
  return label_bb(dst_block);
}

void EmitCtx::emit_comment_if_needed(const sbt::cfg::BundleInst &bi) {
  if (!opt.include_comments) return;
  emit_line("// " + hex_u32(bi.pc) + " " + bi.inst.name);
}

void EmitCtx::validate_shared_preconditions(const sbt::DecodedInst &di) const {
  if (!sbt::is_scalar_exec_classification_required(di)) return;
  (void)scalar_exec_kind_for_inst(di, func_name);
}

void EmitCtx::emit_one_inst(const sbt::cfg::BundleInst &bi) {
  const sbt::DecodedInst &di = bi.inst;
  emit_comment_if_needed(bi);
  validate_shared_preconditions(di);

  if (try_emit_control(*this, bi)) return;
  if (try_emit_scalar(*this, di)) return;
  if (try_emit_vector(*this, di)) return;
  if (try_emit_mma(*this, di)) return;
  if (try_emit_custom(*this, di)) return;

  throw EmitError("unsupported.inst", func_name, di.pc, di.name);
}

void EmitCtx::emit_fallthrough_edge_if_needed(const sbt::cfg::BasicBlock &bb) {
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

void EmitCtx::emit_body() {
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

} // namespace sbt::ptx::detail
