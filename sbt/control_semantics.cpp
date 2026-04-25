#include "sbt/control_semantics.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace sbt::control {
namespace {

std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

std::string inst_label(const cfg::BundleInst &bi) {
  return "'" + bi.inst.name + "' at pc=" + hex_u32(bi.inst_pc);
}

std::runtime_error contract_error(const cfg::BundleInst &bi,
                                  const std::string &detail) {
  return std::runtime_error("control semantics contract error for " +
                            inst_label(bi) + ": " + detail);
}

void require_contract(bool ok, const cfg::BundleInst &bi,
                      const std::string &detail) {
  if (!ok)
    throw contract_error(bi, detail);
}

void validate_no_stray_control_fields(const cfg::BundleInst &bi) {
  const auto &emit = bi.inst.emit;
  if (emit.domain != EmitDomain::Control &&
      emit.control_kind != ControlKind::None) {
    throw contract_error(bi, "non-control domain carries control_kind");
  }
  if (emit.domain != EmitDomain::StructuredControl &&
      emit.structured_control_kind != StructuredControlKind::None) {
    throw contract_error(
        bi, "non-structured domain carries structured_control_kind");
  }
  if (emit.domain != EmitDomain::Control &&
      emit.branch_cond != BranchCondKind::None) {
    throw contract_error(bi, "non-control domain carries branch_cond");
  }
}

int64_t compute_direct_target(const cfg::BundleInst &bi) {
  return static_cast<int64_t>(bi.inst_pc) + static_cast<int64_t>(bi.inst.imm);
}

Semantics classify_control_domain(const cfg::BundleInst &bi) {
  const auto &di = bi.inst;
  const auto &emit = di.emit;
  Semantics out;
  out.domain = emit.domain;
  out.control_kind = emit.control_kind;
  out.branch_cond = emit.branch_cond;

  require_contract(emit.control_kind != ControlKind::None, bi,
                   "missing control_kind for supported control instruction");
  require_contract(
      emit.structured_control_kind == StructuredControlKind::None, bi,
      "control instruction must not carry structured_control_kind");

  switch (emit.control_kind) {
  case ControlKind::ScalarBranch:
    require_contract(emit.branch_cond != BranchCondKind::None, bi,
                     "scalar branch requires branch_cond");
    require_contract(di.rs1_class == RegClass::X && di.rs2_class == RegClass::X,
                     bi, "scalar branch requires X rs1/rs2");
    require_contract(di.imm_kind == ImmKind::B13, bi,
                     "scalar branch requires B13 immediate");
    out.is_scalar_branch = true;
    out.is_terminator = true;
    out.has_fallthrough = true;
    out.has_direct_target = true;
    out.direct_target = compute_direct_target(bi);
    return out;
  case ControlKind::VectorBranch:
    require_contract(emit.branch_cond != BranchCondKind::None, bi,
                     "vector branch requires branch_cond");
    require_contract(di.rs1_class == RegClass::V && di.rs2_class == RegClass::V,
                     bi, "vector branch requires V rs1/rs2");
    require_contract(di.imm_kind == ImmKind::B13, bi,
                     "vector branch requires B13 immediate");
    out.is_vector_branch = true;
    out.is_terminator = true;
    out.has_fallthrough = true;
    out.has_direct_target = true;
    out.direct_target = compute_direct_target(bi);
    return out;
  case ControlKind::DirectJump:
    require_contract(emit.branch_cond == BranchCondKind::None, bi,
                     "direct jump must not carry branch_cond");
    require_contract(di.rd_class == RegClass::X && di.rd == 0, bi,
                     "direct jump requires rd=x0");
    require_contract(di.imm_kind == ImmKind::J21, bi,
                     "direct jump requires J21 immediate");
    out.is_direct_jump = true;
    out.is_terminator = true;
    out.has_fallthrough = false;
    out.has_direct_target = true;
    out.direct_target = compute_direct_target(bi);
    return out;
  case ControlKind::DirectCall:
    require_contract(emit.branch_cond == BranchCondKind::None, bi,
                     "direct call must not carry branch_cond");
    require_contract(di.rd_class == RegClass::X && di.rd != 0, bi,
                     "direct call requires x rd!=0");
    require_contract(di.imm_kind == ImmKind::J21, bi,
                     "direct call requires J21 immediate");
    out.is_direct_call = true;
    out.is_terminator = false;
    out.has_fallthrough = true;
    out.has_direct_target = true;
    out.direct_target = compute_direct_target(bi);
    return out;
  case ControlKind::Return:
    require_contract(emit.branch_cond == BranchCondKind::None, bi,
                     "return must not carry branch_cond");
    require_contract(di.rd_class == RegClass::X && di.rs1_class == RegClass::X,
                     bi, "return requires X rd/rs1");
    require_contract(di.rd == 0 && di.rs1 == 1, bi,
                     "return requires jalr x0, x1, 0");
    require_contract(di.imm_kind == ImmKind::I12 && di.imm == 0, bi,
                     "return requires zero I12 immediate");
    out.is_return = true;
    out.is_terminator = true;
    out.has_fallthrough = false;
    return out;
  case ControlKind::IndirectTerminator:
    require_contract(emit.branch_cond == BranchCondKind::None, bi,
                     "indirect terminator must not carry branch_cond");
    require_contract(di.rd_class == RegClass::X && di.rs1_class == RegClass::X,
                     bi, "indirect terminator requires X rd/rs1");
    require_contract(di.imm_kind == ImmKind::I12, bi,
                     "indirect terminator requires I12 immediate");
    require_contract(!(di.rd == 0 && di.rs1 == 1 && di.imm == 0), bi,
                     "ret-shape jalr must classify as Return");
    out.is_indirect_terminator = true;
    out.is_terminator = true;
    out.has_fallthrough = false;
    return out;
  case ControlKind::None:
    break;
  }

  throw contract_error(bi, "unsupported control_kind enum value");
}

Semantics classify_structured_domain(const cfg::BundleInst &bi) {
  const auto &di = bi.inst;
  const auto &emit = di.emit;
  Semantics out;
  out.domain = emit.domain;
  out.structured_control_kind = emit.structured_control_kind;

  require_contract(emit.control_kind == ControlKind::None, bi,
                   "structured control must not carry control_kind");
  require_contract(emit.branch_cond == BranchCondKind::None, bi,
                   "structured control must not carry branch_cond");
  require_contract(emit.structured_control_kind != StructuredControlKind::None,
                   bi,
                   "missing structured_control_kind for supported "
                   "structured-control instruction");

  switch (emit.structured_control_kind) {
  case StructuredControlKind::SetRpc:
    require_contract(di.rs1_class == RegClass::X && di.rs1 >= 0, bi,
                     "setrpc requires X rs1");
    require_contract(di.imm_kind == ImmKind::I12, bi,
                     "setrpc requires I12 immediate");
    out.is_setrpc = true;
    return out;
  case StructuredControlKind::Join:
    out.is_join = true;
    return out;
  case StructuredControlKind::Barrier:
    out.is_barrier = true;
    return out;
  case StructuredControlKind::Vsetvli:
    out.is_vsetvli = true;
    return out;
  case StructuredControlKind::EndPrg:
    out.is_endprg = true;
    out.is_terminator = true;
    out.has_fallthrough = false;
    return out;
  case StructuredControlKind::None:
    break;
  }

  throw contract_error(bi, "unsupported structured_control_kind enum value");
}

} // namespace

Semantics classify(const cfg::BundleInst &bi) {
  validate_no_stray_control_fields(bi);

  switch (bi.inst.emit.domain) {
  case EmitDomain::Control:
    return classify_control_domain(bi);
  case EmitDomain::StructuredControl:
    return classify_structured_domain(bi);
  default:
    return {};
  }
}

bool is_scalar_auipc(const DecodedInst &di) {
  if (di.emit.domain != EmitDomain::ScalarInteger ||
      di.emit.scalar_int_kind != ScalarIntKind::Auipc)
    return false;
  if (di.rd_class != RegClass::X || di.rd < 0 || di.imm_kind != ImmKind::U20) {
    cfg::BundleInst bi;
    bi.pc = di.pc;
    bi.inst_pc = di.pc;
    bi.inst = di;
    throw contract_error(bi, "auipc metadata requires X rd and U20 immediate");
  }
  return true;
}

std::optional<uint32_t> resolve_setrpc_join_pc(const cfg::FunctionCfg &cfg,
                                               size_t setrpc_inst_idx) {
  const auto &bi = cfg.insts.at(setrpc_inst_idx);
  const auto semantics = classify(bi);
  if (!semantics.is_setrpc)
    return std::nullopt;

  const int rs1 = bi.inst.rs1;
  const int32_t off = bi.inst.imm;
  const size_t begin = (setrpc_inst_idx > 12 ? setrpc_inst_idx - 12 : 0);
  for (size_t j = setrpc_inst_idx; j-- > begin;) {
    const auto &auipc_bi = cfg.insts[j];
    if (!is_scalar_auipc(auipc_bi.inst))
      continue;
    if (auipc_bi.inst.rd != rs1)
      continue;
    const int64_t base = static_cast<int64_t>(auipc_bi.inst_pc) +
                         static_cast<int64_t>(auipc_bi.inst.imm);
    const int64_t join = base + static_cast<int64_t>(off);
    return static_cast<uint32_t>(join);
  }
  return std::nullopt;
}

std::vector<uint32_t> collect_direct_call_targets(const cfg::FunctionCfg &cfg,
                                                  std::string_view owner_name) {
  std::vector<uint32_t> targets;
  for (const auto &bi : cfg.insts) {
    const auto semantics = classify(bi);
    if (!semantics.is_direct_call)
      continue;
    if (semantics.direct_target < 0 ||
        semantics.direct_target > 0xffff'ffffll) {
      const std::string owner = owner_name.empty()
                                    ? std::string()
                                    : (" in " + std::string(owner_name));
      throw std::runtime_error("direct-call target out of range" + owner +
                               " at pc=" + hex_u32(bi.inst_pc));
    }
    targets.push_back(static_cast<uint32_t>(semantics.direct_target));
  }

  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

} // namespace sbt::control
