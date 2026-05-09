#include "sbt/riscv_decode.hpp"

#include <array>
#include <optional>

namespace sbt {
namespace {

constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

EmitDescriptor emit_descriptor_for_name(std::string_view name);

constexpr InstId fnv1a(std::string_view name) {
  uint32_t value = kFnvOffsetBasis;
  for (const char ch : name) {
    value ^= static_cast<uint8_t>(ch);
    value *= kFnvPrime;
  }
  return value;
}

InstMetadata meta(const char *name, OperandForm operand_form, RegClass rd_class, RegClass rs1_class, RegClass rs2_class, RegClass rs3_class,
                  ImmKind imm_kind, UniformTransferKind uniform_transfer_kind, ScalarExecKind scalar_exec_kind, bool spike_managed) {
  return InstMetadata{
      .id = fnv1a(name),
      .name = name,
      .operand_form = operand_form,
      .rd_class = rd_class,
      .rs1_class = rs1_class,
      .rs2_class = rs2_class,
      .rs3_class = rs3_class,
      .imm_kind = imm_kind,
      .uniform_transfer_kind = uniform_transfer_kind,
      .scalar_exec_kind = scalar_exec_kind,
      .emit = emit_descriptor_for_name(name),
      .spike_managed = spike_managed,
  };
}

EmitDescriptor make_structured_control(StructuredControlKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::StructuredControl;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.structured_control_kind = kind;
  return emit;
}

EmitDescriptor make_control(ControlKind kind, BranchCondKind cond = BranchCondKind::None) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::Control;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.control_kind = kind;
  emit.branch_cond = cond;
  return emit;
}

EmitDescriptor make_scalar_memory(MemAccessKind access, MemWidth width, MemValueKind value_kind, MemExtKind ext_kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::ScalarMemory;
  emit.scalar_exec_applicability = ExecClassificationApplicability::Required;
  emit.mem_access_kind = access;
  emit.mem_width = width;
  emit.mem_value_kind = value_kind;
  emit.mem_ext_kind = ext_kind;
  emit.memory_addr_kind = MemoryAddrKind::Ordinary;
  return emit;
}

EmitDescriptor make_scalar_int(ScalarIntKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::ScalarInteger;
  emit.scalar_exec_applicability = ExecClassificationApplicability::Required;
  emit.scalar_int_kind = kind;
  return emit;
}

EmitDescriptor make_scalar_fp(ScalarFpKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::ScalarFp;
  emit.scalar_exec_applicability = ExecClassificationApplicability::Required;
  emit.scalar_fp_kind = kind;
  return emit;
}

EmitDescriptor make_csr(CsrOpKind op_kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::Csr;
  emit.scalar_exec_applicability = ExecClassificationApplicability::Required;
  emit.csr_op_kind = op_kind;
  emit.csr_semantic_class = CsrSemanticClass::VentusReadonly;
  return emit;
}

EmitDescriptor make_vector_memory(MemAccessKind access, MemoryAddrKind addr_kind, MemWidth width, MemValueKind value_kind,
                                  MemExtKind ext_kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorMemory;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.mem_access_kind = access;
  emit.memory_addr_kind = addr_kind;
  emit.mem_width = width;
  emit.mem_value_kind = value_kind;
  emit.mem_ext_kind = ext_kind;
  return emit;
}

EmitDescriptor make_vector_register(VectorRegisterKind kind, ExecClassificationApplicability applicability = ExecClassificationApplicability::NotRequired) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorRegister;
  emit.scalar_exec_applicability = applicability;
  emit.vector_register_kind = kind;
  return emit;
}

EmitDescriptor make_vector_int(VectorIntKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorInteger;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.vector_int_kind = kind;
  return emit;
}

EmitDescriptor make_vector_compare(VectorCompareKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorCompare;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.vector_compare_kind = kind;
  return emit;
}

EmitDescriptor make_vector_mask(VectorMaskKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorMask;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.vector_mask_kind = kind;
  return emit;
}

EmitDescriptor make_vector_convert(VectorConvertKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorConvert;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.vector_convert_kind = kind;
  return emit;
}

EmitDescriptor make_vector_fp(VectorFpKind kind) {
  EmitDescriptor emit;
  emit.domain = EmitDomain::VectorFp;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  emit.vector_fp_kind = kind;
  return emit;
}

std::optional<EmitDescriptor> structured_control_descriptor_for_name(std::string_view name) {
  if (name == "setrpc") return make_structured_control(StructuredControlKind::SetRpc);
  if (name == "join") return make_structured_control(StructuredControlKind::Join);
  if (name == "barrier") return make_structured_control(StructuredControlKind::Barrier);
  if (name == "vsetvli") return make_structured_control(StructuredControlKind::Vsetvli);
  if (name == "endprg") return make_structured_control(StructuredControlKind::EndPrg);
  return std::nullopt;
}

std::optional<EmitDescriptor> control_descriptor_for_name(std::string_view name) {
  if (name == "beq") return make_control(ControlKind::ScalarBranch, BranchCondKind::Eq);
  if (name == "bne") return make_control(ControlKind::ScalarBranch, BranchCondKind::Ne);
  if (name == "blt") return make_control(ControlKind::ScalarBranch, BranchCondKind::Lt);
  if (name == "bge") return make_control(ControlKind::ScalarBranch, BranchCondKind::Ge);
  if (name == "bltu") return make_control(ControlKind::ScalarBranch, BranchCondKind::Ltu);
  if (name == "bgeu") return make_control(ControlKind::ScalarBranch, BranchCondKind::Geu);
  if (name == "vbeq") return make_control(ControlKind::VectorBranch, BranchCondKind::Eq);
  if (name == "vbne") return make_control(ControlKind::VectorBranch, BranchCondKind::Ne);
  if (name == "vblt") return make_control(ControlKind::VectorBranch, BranchCondKind::Lt);
  if (name == "vbge") return make_control(ControlKind::VectorBranch, BranchCondKind::Ge);
  if (name == "vbltu") return make_control(ControlKind::VectorBranch, BranchCondKind::Ltu);
  if (name == "vbgeu") return make_control(ControlKind::VectorBranch, BranchCondKind::Geu);
  if (name == "jal") return make_control(ControlKind::DirectJump);
  if (name == "jalr") return make_control(ControlKind::IndirectTerminator);
  return std::nullopt;
}

std::optional<EmitDescriptor> scalar_memory_descriptor_for_name(std::string_view name) {
  if (name == "lb") return make_scalar_memory(MemAccessKind::Load, MemWidth::Byte, MemValueKind::Integer, MemExtKind::SignExtend);
  if (name == "lbu") return make_scalar_memory(MemAccessKind::Load, MemWidth::Byte, MemValueKind::Integer, MemExtKind::ZeroExtend);
  if (name == "lh") return make_scalar_memory(MemAccessKind::Load, MemWidth::Half, MemValueKind::Integer, MemExtKind::SignExtend);
  if (name == "lhu") return make_scalar_memory(MemAccessKind::Load, MemWidth::Half, MemValueKind::Integer, MemExtKind::ZeroExtend);
  if (name == "lw") return make_scalar_memory(MemAccessKind::Load, MemWidth::Word, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "flw") return make_scalar_memory(MemAccessKind::Load, MemWidth::Word, MemValueKind::FloatBits, MemExtKind::RawBits);
  if (name == "sb") return make_scalar_memory(MemAccessKind::Store, MemWidth::Byte, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "sh") return make_scalar_memory(MemAccessKind::Store, MemWidth::Half, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "sw") return make_scalar_memory(MemAccessKind::Store, MemWidth::Word, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "fsw") return make_scalar_memory(MemAccessKind::Store, MemWidth::Word, MemValueKind::FloatBits, MemExtKind::RawBits);
  return std::nullopt;
}

std::optional<EmitDescriptor> scalar_int_descriptor_for_name(std::string_view name) {
  if (name == "addi" || name == "add") return make_scalar_int(ScalarIntKind::Add);
  if (name == "sub") return make_scalar_int(ScalarIntKind::Sub);
  if (name == "and" || name == "andi") return make_scalar_int(ScalarIntKind::And);
  if (name == "or" || name == "ori") return make_scalar_int(ScalarIntKind::Or);
  if (name == "xor" || name == "xori") return make_scalar_int(ScalarIntKind::Xor);
  if (name == "mul") return make_scalar_int(ScalarIntKind::Mul);
  if (name == "mulh") return make_scalar_int(ScalarIntKind::MulH);
  if (name == "mulhu") return make_scalar_int(ScalarIntKind::MulHU);
  if (name == "mulhsu") return make_scalar_int(ScalarIntKind::MulHSU);
  if (name == "div") return make_scalar_int(ScalarIntKind::Div);
  if (name == "divu") return make_scalar_int(ScalarIntKind::DivU);
  if (name == "rem") return make_scalar_int(ScalarIntKind::Rem);
  if (name == "remu") return make_scalar_int(ScalarIntKind::RemU);
  if (name == "lui") return make_scalar_int(ScalarIntKind::Lui);
  if (name == "auipc") return make_scalar_int(ScalarIntKind::Auipc);
  if (name == "slli" || name == "sll") return make_scalar_int(ScalarIntKind::ShiftLeft);
  if (name == "srli" || name == "srl") return make_scalar_int(ScalarIntKind::ShiftRightLogical);
  if (name == "srai" || name == "sra") return make_scalar_int(ScalarIntKind::ShiftRightArithmetic);
  if (name == "slt" || name == "slti") return make_scalar_int(ScalarIntKind::SetLessThan);
  if (name == "sltu" || name == "sltiu") return make_scalar_int(ScalarIntKind::SetLessThanU);
  return std::nullopt;
}

std::optional<EmitDescriptor> scalar_fp_descriptor_for_name(std::string_view name) {
  if (name == "fmv_w_x" || name == "fmv_x_w") {
    auto emit = make_scalar_fp(ScalarFpKind::MoveBits);
    return emit;
  }
  if (name == "fsgnj_s" || name == "fsgnjn_s" || name == "fsgnjx_s") {
    auto emit = make_scalar_fp(ScalarFpKind::SignInject);
    emit.fp_sign_inject_kind = name == "fsgnj_s" ? FpSignInjectKind::CopySign
                             : name == "fsgnjn_s" ? FpSignInjectKind::NegateSign
                                                   : FpSignInjectKind::XorSign;
    return emit;
  }
  if (name == "fadd_s" || name == "fsub_s" || name == "fmul_s" || name == "fdiv_s") {
    auto emit = make_scalar_fp(ScalarFpKind::Binary);
    emit.fp_binary_kind = name == "fadd_s" ? FpBinaryKind::Add
                        : name == "fsub_s" ? FpBinaryKind::Sub
                        : name == "fmul_s" ? FpBinaryKind::Mul
                                           : FpBinaryKind::Div;
    return emit;
  }
  if (name == "fsqrt_s") return make_scalar_fp(ScalarFpKind::Sqrt);
  if (name == "fmadd_s" || name == "fmsub_s" || name == "fnmsub_s" || name == "fnmadd_s") {
    auto emit = make_scalar_fp(ScalarFpKind::Fma);
    emit.fp_ternary_kind = name == "fmadd_s" ? FpTernaryKind::MAdd
                         : name == "fmsub_s" ? FpTernaryKind::MSub
                         : name == "fnmsub_s" ? FpTernaryKind::NMSub
                                              : FpTernaryKind::NMAdd;
    return emit;
  }
  if (name == "fmin_s" || name == "fmax_s") {
    auto emit = make_scalar_fp(ScalarFpKind::MinMax);
    emit.fp_minmax_kind = name == "fmin_s" ? FpMinMaxKind::Min : FpMinMaxKind::Max;
    return emit;
  }
  if (name == "feq_s" || name == "flt_s" || name == "fle_s") {
    auto emit = make_scalar_fp(ScalarFpKind::Compare);
    emit.fp_compare_kind = name == "feq_s" ? FpCompareKind::Eq
                          : name == "flt_s" ? FpCompareKind::Lt
                                            : FpCompareKind::Le;
    return emit;
  }
  if (name == "fcvt_s_w" || name == "fcvt_s_wu" || name == "fcvt_w_s" || name == "fcvt_wu_s") {
    auto emit = make_scalar_fp(ScalarFpKind::Convert);
    emit.fp_convert_kind = name == "fcvt_s_w" ? FpConvertKind::IntToFloatSigned
                           : name == "fcvt_s_wu" ? FpConvertKind::IntToFloatUnsigned
                           : name == "fcvt_w_s" ? FpConvertKind::FloatToIntSigned
                                                : FpConvertKind::FloatToIntUnsigned;
    return emit;
  }
  if (name == "fclass_s") return make_scalar_fp(ScalarFpKind::Classify);
  return std::nullopt;
}

std::optional<EmitDescriptor> csr_descriptor_for_name(std::string_view name) {
  if (name == "csrrw") return make_csr(CsrOpKind::Rw);
  if (name == "csrrs") return make_csr(CsrOpKind::Rs);
  if (name == "csrrc") return make_csr(CsrOpKind::Rc);
  if (name == "csrrwi") return make_csr(CsrOpKind::Rwi);
  if (name == "csrrsi") return make_csr(CsrOpKind::Rsi);
  if (name == "csrrci") return make_csr(CsrOpKind::Rci);
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_memory_descriptor_for_name(std::string_view name) {
  if (name == "vlw_v") return make_vector_memory(MemAccessKind::Load, MemoryAddrKind::Pds, MemWidth::Word, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "vsb_v") return make_vector_memory(MemAccessKind::Store, MemoryAddrKind::Pds, MemWidth::Byte, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "vsw_v") return make_vector_memory(MemAccessKind::Store, MemoryAddrKind::Pds, MemWidth::Word, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "vlb12_v") return make_vector_memory(MemAccessKind::Load, MemoryAddrKind::Ordinary, MemWidth::Byte, MemValueKind::Integer, MemExtKind::SignExtend);
  if (name == "vlbu12_v") return make_vector_memory(MemAccessKind::Load, MemoryAddrKind::Ordinary, MemWidth::Byte, MemValueKind::Integer, MemExtKind::ZeroExtend);
  if (name == "vlh12_v") return make_vector_memory(MemAccessKind::Load, MemoryAddrKind::Ordinary, MemWidth::Half, MemValueKind::Integer, MemExtKind::SignExtend);
  if (name == "vlhu12_v") return make_vector_memory(MemAccessKind::Load, MemoryAddrKind::Ordinary, MemWidth::Half, MemValueKind::Integer, MemExtKind::ZeroExtend);
  if (name == "vlw12_v") return make_vector_memory(MemAccessKind::Load, MemoryAddrKind::Ordinary, MemWidth::Word, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "vsb12_v") return make_vector_memory(MemAccessKind::Store, MemoryAddrKind::Ordinary, MemWidth::Byte, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "vsh12_v") return make_vector_memory(MemAccessKind::Store, MemoryAddrKind::Ordinary, MemWidth::Half, MemValueKind::Integer, MemExtKind::RawBits);
  if (name == "vsw12_v") return make_vector_memory(MemAccessKind::Store, MemoryAddrKind::Ordinary, MemWidth::Word, MemValueKind::Integer, MemExtKind::RawBits);
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_register_descriptor_for_name(std::string_view name) {
  if (name == "vmv_v_x" || name == "vfmv_v_f") return make_vector_register(VectorRegisterKind::BroadcastScalar);
  if (name == "vmv_s_x") return make_vector_register(VectorRegisterKind::InsertScalar);
  if (name == "vmv_v_i") return make_vector_register(VectorRegisterKind::BroadcastImmediate);
  if (name == "vmv_v_v") return make_vector_register(VectorRegisterKind::MoveVector);
  if (name == "vmv_x_s") return make_vector_register(VectorRegisterKind::ExtractScalar, ExecClassificationApplicability::Required);
  if (name == "vid_v") return make_vector_register(VectorRegisterKind::LaneId);
  if (name == "vmerge_vvm" || name == "vmerge_vxm" || name == "vmerge_vim" || name == "vfmerge_vfm") {
    auto emit = make_vector_register(VectorRegisterKind::Merge);
    emit.merge_kind = name == "vmerge_vvm" ? MergeKind::Vvm
                    : name == "vmerge_vxm" ? MergeKind::Vxm
                    : name == "vmerge_vim" ? MergeKind::Vim
                                           : MergeKind::Vfm;
    return emit;
  }
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_int_descriptor_for_name(std::string_view name) {
  if (name == "vadd_vv" || name == "vadd_vx" || name == "vadd_vi" || name == "vadd12_vi") return make_vector_int(VectorIntKind::Add);
  if (name == "vsub_vv" || name == "vsub_vx" || name == "vsub12_vi") return make_vector_int(VectorIntKind::Sub);
  if (name == "vrsub_vi" || name == "vrsub_vx") return make_vector_int(VectorIntKind::RSub);
  if (name == "vmin_vv" || name == "vmin_vx") return make_vector_int(VectorIntKind::Min);
  if (name == "vminu_vv" || name == "vminu_vx") return make_vector_int(VectorIntKind::MinU);
  if (name == "vmax_vv" || name == "vmax_vx") return make_vector_int(VectorIntKind::Max);
  if (name == "vmaxu_vv" || name == "vmaxu_vx") return make_vector_int(VectorIntKind::MaxU);
  if (name == "vand_vv" || name == "vand_vx" || name == "vand_vi") return make_vector_int(VectorIntKind::And);
  if (name == "vor_vv" || name == "vor_vx" || name == "vor_vi") return make_vector_int(VectorIntKind::Or);
  if (name == "vxor_vv" || name == "vxor_vx" || name == "vxor_vi") return make_vector_int(VectorIntKind::Xor);
  if (name == "vsll_vv" || name == "vsll_vx" || name == "vsll_vi") return make_vector_int(VectorIntKind::ShiftLeft);
  if (name == "vsrl_vv" || name == "vsrl_vx" || name == "vsrl_vi") return make_vector_int(VectorIntKind::ShiftRightLogical);
  if (name == "vsra_vv" || name == "vsra_vx" || name == "vsra_vi") return make_vector_int(VectorIntKind::ShiftRightArithmetic);
  if (name == "vmul_vv" || name == "vmul_vx") return make_vector_int(VectorIntKind::Mul);
  if (name == "vmulh_vv" || name == "vmulh_vx") return make_vector_int(VectorIntKind::MulH);
  if (name == "vmulhu_vv" || name == "vmulhu_vx") return make_vector_int(VectorIntKind::MulHU);
  if (name == "vmulhsu_vv" || name == "vmulhsu_vx") return make_vector_int(VectorIntKind::MulHSU);
  if (name == "vdiv_vv" || name == "vdiv_vx") return make_vector_int(VectorIntKind::Div);
  if (name == "vdivu_vv" || name == "vdivu_vx") return make_vector_int(VectorIntKind::DivU);
  if (name == "vrem_vv" || name == "vrem_vx") return make_vector_int(VectorIntKind::Rem);
  if (name == "vremu_vv" || name == "vremu_vx") return make_vector_int(VectorIntKind::RemU);
  if (name == "vmadd_vv" || name == "vmadd_vx") return make_vector_int(VectorIntKind::Madd);
  if (name == "vnmsub_vv" || name == "vnmsub_vx") return make_vector_int(VectorIntKind::NMSub);
  if (name == "vmacc_vv" || name == "vmacc_vx") return make_vector_int(VectorIntKind::MAcc);
  if (name == "vnmsac_vv" || name == "vnmsac_vx") return make_vector_int(VectorIntKind::NMSac);
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_compare_descriptor_for_name(std::string_view name) {
  if (name == "vmseq_vv" || name == "vmseq_vx" || name == "vmseq_vi") return make_vector_compare(VectorCompareKind::Eq);
  if (name == "vmsne_vv" || name == "vmsne_vx" || name == "vmsne_vi") return make_vector_compare(VectorCompareKind::Ne);
  if (name == "vmslt_vv" || name == "vmslt_vx") return make_vector_compare(VectorCompareKind::Lt);
  if (name == "vmsltu_vv" || name == "vmsltu_vx") return make_vector_compare(VectorCompareKind::LtU);
  if (name == "vmsle_vv" || name == "vmsle_vx" || name == "vmsle_vi") return make_vector_compare(VectorCompareKind::Le);
  if (name == "vmsleu_vv" || name == "vmsleu_vx" || name == "vmsleu_vi") return make_vector_compare(VectorCompareKind::LeU);
  if (name == "vmsgt_vx" || name == "vmsgt_vi") return make_vector_compare(VectorCompareKind::Gt);
  if (name == "vmsgtu_vx" || name == "vmsgtu_vi") return make_vector_compare(VectorCompareKind::GtU);
  if (name == "vmfeq_vv" || name == "vmfeq_vf") return make_vector_compare(VectorCompareKind::FEq);
  if (name == "vmfle_vv" || name == "vmfle_vf") return make_vector_compare(VectorCompareKind::FLe);
  if (name == "vmflt_vv" || name == "vmflt_vf") return make_vector_compare(VectorCompareKind::FLt);
  if (name == "vmfgt_vf") return make_vector_compare(VectorCompareKind::FGt);
  if (name == "vmfge_vf") return make_vector_compare(VectorCompareKind::FGe);
  if (name == "vmfne_vv" || name == "vmfne_vf") return make_vector_compare(VectorCompareKind::FNe);
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_mask_descriptor_for_name(std::string_view name) {
  if (name == "vmand_mm") return make_vector_mask(VectorMaskKind::And);
  if (name == "vmandn_mm") return make_vector_mask(VectorMaskKind::AndNot);
  if (name == "vmor_mm") return make_vector_mask(VectorMaskKind::Or);
  if (name == "vmorn_mm") return make_vector_mask(VectorMaskKind::OrNot);
  if (name == "vmxor_mm") return make_vector_mask(VectorMaskKind::Xor);
  if (name == "vmxnor_mm") return make_vector_mask(VectorMaskKind::XNor);
  if (name == "vmnand_mm") return make_vector_mask(VectorMaskKind::Nand);
  if (name == "vmnor_mm") return make_vector_mask(VectorMaskKind::Nor);
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_convert_descriptor_for_name(std::string_view name) {
  if (name == "vfcvt_f_x_v") return make_vector_convert(VectorConvertKind::FloatFromInt);
  if (name == "vfcvt_f_xu_v") return make_vector_convert(VectorConvertKind::FloatFromUInt);
  if (name == "vfcvt_x_f_v") return make_vector_convert(VectorConvertKind::IntFromFloat);
  if (name == "vfcvt_xu_f_v") return make_vector_convert(VectorConvertKind::UIntFromFloat);
  if (name == "vfcvt_rtz_x_f_v") return make_vector_convert(VectorConvertKind::IntFromFloatRtz);
  if (name == "vfcvt_rtz_xu_f_v") return make_vector_convert(VectorConvertKind::UIntFromFloatRtz);
  if (name == "vfclass_v") return make_vector_convert(VectorConvertKind::Classify);
  return std::nullopt;
}

std::optional<EmitDescriptor> vector_fp_descriptor_for_name(std::string_view name) {
  if (name == "vfexp_v") return make_vector_fp(VectorFpKind::Exp);
  if (name == "vfadd_vv" || name == "vfadd_vf") return make_vector_fp(VectorFpKind::Add);
  if (name == "vfsub_vv" || name == "vfsub_vf") return make_vector_fp(VectorFpKind::Sub);
  if (name == "vfmul_vv" || name == "vfmul_vf") return make_vector_fp(VectorFpKind::Mul);
  if (name == "vfdiv_vv" || name == "vfdiv_vf") return make_vector_fp(VectorFpKind::Div);
  if (name == "vfrsub_vf") return make_vector_fp(VectorFpKind::RSub);
  if (name == "vfrdiv_vf") return make_vector_fp(VectorFpKind::RDiv);
  if (name == "vfmin_vv" || name == "vfmin_vf") return make_vector_fp(VectorFpKind::Min);
  if (name == "vfmax_vv" || name == "vfmax_vf") return make_vector_fp(VectorFpKind::Max);
  if (name == "vfmadd_vv" || name == "vfmadd_vf") return make_vector_fp(VectorFpKind::MAdd);
  if (name == "vfmsub_vv" || name == "vfmsub_vf") return make_vector_fp(VectorFpKind::MSub);
  if (name == "vfnmadd_vv" || name == "vfnmadd_vf") return make_vector_fp(VectorFpKind::NMAdd);
  if (name == "vfnmsub_vv" || name == "vfnmsub_vf") return make_vector_fp(VectorFpKind::NMSub);
  if (name == "vfmacc_vv" || name == "vfmacc_vf") return make_vector_fp(VectorFpKind::MAcc);
  if (name == "vfnmacc_vv" || name == "vfnmacc_vf") return make_vector_fp(VectorFpKind::NMAcc);
  if (name == "vfmsac_vv" || name == "vfmsac_vf") return make_vector_fp(VectorFpKind::MSac);
  if (name == "vfnmsac_vv" || name == "vfnmsac_vf") return make_vector_fp(VectorFpKind::NMSac);
  if (name == "vfsqrt_v") return make_vector_fp(VectorFpKind::Sqrt);
  if (name == "vfsgnj_vv" || name == "vfsgnj_vf" || name == "vfsgnjn_vv" || name == "vfsgnjn_vf" || name == "vfsgnjx_vv" ||
      name == "vfsgnjx_vf") {
    auto emit = make_vector_fp(VectorFpKind::SignInject);
    emit.vector_fp_sign_inject_kind = (name == "vfsgnj_vv" || name == "vfsgnj_vf") ? VectorFpSignInjectKind::CopySign
                                   : (name == "vfsgnjn_vv" || name == "vfsgnjn_vf") ? VectorFpSignInjectKind::NegateSign
                                                                                     : VectorFpSignInjectKind::XorSign;
    return emit;
  }
  return std::nullopt;
}

EmitDescriptor emit_descriptor_for_name(std::string_view name) {
  if (const auto emit = structured_control_descriptor_for_name(name)) return *emit;
  if (const auto emit = control_descriptor_for_name(name)) return *emit;
  if (const auto emit = scalar_memory_descriptor_for_name(name)) return *emit;
  if (const auto emit = scalar_int_descriptor_for_name(name)) return *emit;
  if (const auto emit = scalar_fp_descriptor_for_name(name)) return *emit;
  if (const auto emit = csr_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_memory_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_register_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_int_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_compare_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_mask_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_convert_descriptor_for_name(name)) return *emit;
  if (const auto emit = vector_fp_descriptor_for_name(name)) return *emit;

  EmitDescriptor emit;
  emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
  return emit;
}

#define META(name, operand_form, rd_class, rs1_class, rs2_class, rs3_class, imm_kind, uniform_transfer_kind, scalar_exec_kind, spike_managed) \
  meta(name, operand_form, rd_class, rs1_class, rs2_class, rs3_class, imm_kind, uniform_transfer_kind, scalar_exec_kind, spike_managed)

static const auto kInstMetadata = std::to_array<InstMetadata>({
    META("barrier", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("endprg", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("join", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("regext", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::Raw12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("regexti", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::Raw12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("setrpc", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vsetvli", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::Raw12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),

    META("vbeq", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vbge", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vbgeu", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vblt", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vbltu", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vbne", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),

    META("vlb12_v", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),
    META("vlbu12_v", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),
    META("vlh12_v", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),
    META("vlhu12_v", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),
    META("vlw12_v", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),
    META("vlw_v", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),

    META("vsb12_v", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vsh12_v", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vsw12_v", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vsb_v", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),
    META("vsw_v", OperandForm::VRs1VectorRs2VectorImm, RegClass::None, RegClass::V, RegClass::V, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::None, true),

    META("vadd12_vi", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),
    META("vsub12_vi", OperandForm::VRdRs1VectorImm, RegClass::V, RegClass::V, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::UniformIfRs1, ScalarExecKind::None, true),

    META("vid_v", OperandForm::VRd, RegClass::V, RegClass::None, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vfmv_v_f", OperandForm::VRdRs1Scalar, RegClass::V, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::AlwaysUniformDst, ScalarExecKind::None, true),
    META("vmv_s_x", OperandForm::VRdRs1Scalar, RegClass::V, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmv_v_i", OperandForm::VRdImm, RegClass::V, RegClass::None, RegClass::None, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::AlwaysUniformDst, ScalarExecKind::None, true),
    META("vmv_v_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmv_v_x", OperandForm::VRdRs1Scalar, RegClass::V, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::AlwaysUniformDst, ScalarExecKind::None, true),
    META("vmv_x_s", OperandForm::XRdRs2Vector, RegClass::X, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::FixedLaneSensitive, true),

    META("vmerge_vim", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmerge_vvm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmerge_vxm", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vfmerge_vfm", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),

    META("vmand_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmandn_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmnand_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmnor_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmor_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmorn_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmxnor_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmxor_mm", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),

    META("vadd_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vand_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vdiv_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vdiv_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vdivu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vdivu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfadd_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfadd_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfclass_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfcvt_f_x_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfcvt_f_xu_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfcvt_rtz_x_f_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfcvt_rtz_xu_f_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfcvt_x_f_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfcvt_xu_f_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfdiv_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfdiv_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfexp_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmacc_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmacc_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfmadd_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vfmadd_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vfmax_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmax_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfmin_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmin_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfmsac_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmsac_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfmsub_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmsub_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfmul_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfmul_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfnmacc_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfnmacc_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfnmadd_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfnmadd_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfnmsac_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfnmsac_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfnmsub_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfnmsub_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfrdiv_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfrsub_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfsgnj_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfsgnj_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfsgnjn_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfsgnjn_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfsgnjx_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfsgnjx_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vfsqrt_v", OperandForm::VRdRs2Vector, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfsub_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vfsub_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmacc_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmacc_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmadd_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmadd_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::NeverUniformDst, ScalarExecKind::None, true),
    META("vmax_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmax_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmaxu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmaxu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmfeq_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmfeq_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmfge_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmfgt_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmfle_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmfle_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmflt_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmflt_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmfne_vf", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmfne_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmin_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmin_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vminu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vminu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmseq_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmseq_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmseq_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsgt_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsgt_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsgtu_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsgtu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsle_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsle_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmsle_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsleu_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsleu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmsleu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmslt_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmslt_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsltu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmsltu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsne_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmsne_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmsne_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmul_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmul_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmulh_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmulh_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmulhsu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmulhsu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vmulhu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vmulhu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vnmsac_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vnmsac_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vnmsub_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vnmsub_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vor_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vor_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vor_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vrem_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vrem_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vremu_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vremu_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vrsub_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vrsub_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsll_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::UImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsll_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vsll_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsra_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::UImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsra_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vsra_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsrl_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::UImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsrl_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vsrl_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vsub_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vsub_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vxor_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vxor_vv", OperandForm::VRdRs2VectorRs1Vector, RegClass::V, RegClass::V, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs1AndRs2, ScalarExecKind::None, true),
    META("vxor_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vadd_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vadd_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vand_vi", OperandForm::VRdRs2VectorImm, RegClass::V, RegClass::None, RegClass::V, RegClass::None, ImmKind::SImm5,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),
    META("vand_vx", OperandForm::VRdRs2VectorRs1Scalar, RegClass::V, RegClass::X, RegClass::V, RegClass::None, ImmKind::None,
         UniformTransferKind::UniformIfRs2, ScalarExecKind::None, true),

    META("add", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("addi", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("and", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("andi", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("auipc", OperandForm::XRdImm, RegClass::X, RegClass::None, RegClass::None, RegClass::None, ImmKind::U20,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("beq", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("bge", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("bgeu", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("blt", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("bltu", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("bne", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::B13,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("csrrc", OperandForm::XRdCsrRs1, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::CSR12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("csrrci", OperandForm::XRdCsrImm, RegClass::X, RegClass::None, RegClass::None, RegClass::None, ImmKind::CSR12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("csrrs", OperandForm::XRdCsrRs1, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::CSR12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("csrrsi", OperandForm::XRdCsrImm, RegClass::X, RegClass::None, RegClass::None, RegClass::None, ImmKind::CSR12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("csrrw", OperandForm::XRdCsrRs1, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::CSR12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("csrrwi", OperandForm::XRdCsrImm, RegClass::X, RegClass::None, RegClass::None, RegClass::None, ImmKind::CSR12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("div", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("divu", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fadd_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fclass_s", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fcvt_s_w", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fcvt_s_wu", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fcvt_w_s", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fcvt_wu_s", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fdiv_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("feq_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fle_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("flt_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("flw", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmadd_s", OperandForm::XRdRs1Rs2Rs3, RegClass::X, RegClass::X, RegClass::X, RegClass::X, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmax_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmin_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmsub_s", OperandForm::XRdRs1Rs2Rs3, RegClass::X, RegClass::X, RegClass::X, RegClass::X, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmul_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmv_w_x", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fmv_x_w", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fnmadd_s", OperandForm::XRdRs1Rs2Rs3, RegClass::X, RegClass::X, RegClass::X, RegClass::X, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fnmsub_s", OperandForm::XRdRs1Rs2Rs3, RegClass::X, RegClass::X, RegClass::X, RegClass::X, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fsgnj_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fsgnjn_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fsgnjx_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fsqrt_s", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fsub_s", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("fsw", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::ExternallySideEffecting, false),
    META("jal", OperandForm::XRdImm, RegClass::X, RegClass::None, RegClass::None, RegClass::None, ImmKind::J21,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("jalr", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("lb", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("lbu", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("lh", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("lhu", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("lui", OperandForm::XRdImm, RegClass::X, RegClass::None, RegClass::None, RegClass::None, ImmKind::U20,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("lw", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("mul", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("mulh", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("mulhsu", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("mulhu", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("or", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("ori", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("rem", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("remu", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("sb", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::ExternallySideEffecting, false),
    META("sh", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::ExternallySideEffecting, false),
    META("sll", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("slli", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("slt", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("slti", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("sltiu", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("sltu", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("sra", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("srai", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("srl", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("srli", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("sub", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("sw", OperandForm::XRs1Rs2Imm, RegClass::None, RegClass::X, RegClass::X, RegClass::None, ImmKind::S12,
         UniformTransferKind::NotApplicable, ScalarExecKind::ExternallySideEffecting, false),
    META("xor", OperandForm::XRdRs1Rs2, RegClass::X, RegClass::X, RegClass::X, RegClass::None, ImmKind::None,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
    META("xori", OperandForm::XRdRs1Imm, RegClass::X, RegClass::X, RegClass::None, RegClass::None, ImmKind::I12,
         UniformTransferKind::NotApplicable, ScalarExecKind::UniformPure, false),
});

#undef META

} // namespace

InstId make_inst_id(std::string_view name) { return fnv1a(name); }

std::span<const InstMetadata> all_inst_metadata() { return std::span<const InstMetadata>(kInstMetadata); }

const InstMetadata *find_inst_metadata(std::string_view name) {
  for (const auto &metadata : kInstMetadata) {
    if (metadata.name == name) return &metadata;
  }
  return nullptr;
}

const InstMetadata *find_inst_metadata(InstId id) {
  if (id == kUnknownInstId) return nullptr;
  for (const auto &metadata : kInstMetadata) {
    if (metadata.id == id) return &metadata;
  }
  return nullptr;
}

void apply_inst_metadata(const InstMetadata &metadata, DecodedInst &out) {
  out.inst_id = metadata.id;
  out.operand_form = metadata.operand_form;
  out.rd_class = metadata.rd_class;
  out.rs1_class = metadata.rs1_class;
  out.rs2_class = metadata.rs2_class;
  out.rs3_class = metadata.rs3_class;
  out.imm_kind = metadata.imm_kind;
  out.uniform_transfer_kind = metadata.uniform_transfer_kind;
  out.scalar_exec_kind = metadata.scalar_exec_kind;
  out.emit = metadata.emit;
}

bool populate_inst_metadata(std::string_view name, DecodedInst &out) {
  if (const auto *metadata = find_inst_metadata(name)) {
    apply_inst_metadata(*metadata, out);
    return true;
  }
  return false;
}

void finalize_emit_descriptor(DecodedInst &out) {
  if (out.name == "unknown" || out.name == "system" || out.name == "regext" || out.name == "regexti") {
    out.emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
    return;
  }

  if (out.mma.valid) {
    out.emit.domain = EmitDomain::Mma;
    out.emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
    return;
  }

  if (out.custom.valid) {
    out.emit.domain = EmitDomain::Custom;
    out.emit.scalar_exec_applicability = ExecClassificationApplicability::NotRequired;
    return;
  }

  if (out.emit.domain == EmitDomain::Control && out.operand_form == OperandForm::XRdImm && out.imm_kind == ImmKind::J21 &&
      out.rd_class == RegClass::X) {
    out.emit.control_kind = (out.rd == 0) ? ControlKind::DirectJump : ControlKind::DirectCall;
  }

  if (out.emit.domain == EmitDomain::Control && out.operand_form == OperandForm::XRdRs1Imm && out.imm_kind == ImmKind::I12 &&
      out.rd_class == RegClass::X && out.rs1_class == RegClass::X) {
    out.emit.control_kind = (out.rd == 0 && out.rs1 == 1 && out.imm == 0) ? ControlKind::Return : ControlKind::IndirectTerminator;
  }
}

bool is_scalar_exec_classification_required(const DecodedInst &di) {
  if (di.emit.scalar_exec_applicability == ExecClassificationApplicability::Required) return true;
  if (di.emit.scalar_exec_applicability == ExecClassificationApplicability::NotRequired) return false;
  const bool has_vector_operand =
      di.rd_class == RegClass::V || di.rs1_class == RegClass::V || di.rs2_class == RegClass::V || di.rs3_class == RegClass::V;
  return !has_vector_operand;
}

const char *to_string(OperandForm form) {
  switch (form) {
  case OperandForm::XRdRs1Imm: return "x-rd-rs1-imm";
  case OperandForm::XRdRs1Rs2: return "x-rd-rs1-rs2";
  case OperandForm::XRdRs1Rs2Rs3: return "x-rd-rs1-rs2-rs3";
  case OperandForm::XRdImm: return "x-rd-imm";
  case OperandForm::XRdCsrRs1: return "x-rd-csr-rs1";
  case OperandForm::XRdCsrImm: return "x-rd-csr-imm";
  case OperandForm::XRs1Rs2Imm: return "x-rs1-rs2-imm";
  case OperandForm::VRd: return "v-rd";
  case OperandForm::VRdImm: return "v-rd-imm";
  case OperandForm::VRdRs1Scalar: return "v-rd-rs1-scalar";
  case OperandForm::VRdRs1VectorImm: return "v-rd-rs1-vector-imm";
  case OperandForm::VRdRs2Vector: return "v-rd-rs2-vector";
  case OperandForm::VRdRs2VectorRs1Scalar: return "v-rd-rs2-vector-rs1-scalar";
  case OperandForm::VRdRs2VectorRs1Vector: return "v-rd-rs2-vector-rs1-vector";
  case OperandForm::VRdRs2VectorImm: return "v-rd-rs2-vector-imm";
  case OperandForm::XRdRs2Vector: return "x-rd-rs2-vector";
  case OperandForm::VRs1VectorRs2VectorImm: return "v-rs1-vector-rs2-vector-imm";
  case OperandForm::None: default: return "none";
  }
}

const char *to_string(UniformTransferKind kind) {
  switch (kind) {
  case UniformTransferKind::NotApplicable: return "not-applicable";
  case UniformTransferKind::AlwaysUniformDst: return "always-uniform-dst";
  case UniformTransferKind::NeverUniformDst: return "never-uniform-dst";
  case UniformTransferKind::UniformIfRs1: return "uniform-if-rs1";
  case UniformTransferKind::UniformIfRs2: return "uniform-if-rs2";
  case UniformTransferKind::UniformIfRs1AndRs2: return "uniform-if-rs1-and-rs2";
  case UniformTransferKind::Unknown: default: return "unknown";
  }
}

const char *to_string(ScalarExecKind kind) {
  switch (kind) {
  case ScalarExecKind::UniformPure: return "uniform-pure";
  case ScalarExecKind::LaneSensitive: return "lane-sensitive";
  case ScalarExecKind::FixedLaneSensitive: return "fixed-lane-sensitive";
  case ScalarExecKind::ExternallySideEffecting: return "externally-side-effecting";
  case ScalarExecKind::None: default: return "none";
  }
}

} // namespace sbt
