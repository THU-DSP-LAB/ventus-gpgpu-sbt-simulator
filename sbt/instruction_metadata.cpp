#include "sbt/riscv_decode.hpp"

#include <array>

namespace sbt {
namespace {

constexpr uint32_t kFnvOffsetBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

constexpr InstId fnv1a(std::string_view name) {
  uint32_t value = kFnvOffsetBasis;
  for (const char ch : name) {
    value ^= static_cast<uint8_t>(ch);
    value *= kFnvPrime;
  }
  return value;
}

constexpr InstMetadata meta(const char *name, OperandForm operand_form, RegClass rd_class, RegClass rs1_class, RegClass rs2_class,
                            RegClass rs3_class, ImmKind imm_kind, UniformTransferKind uniform_transfer_kind,
                            ScalarExecKind scalar_exec_kind, bool spike_managed) {
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
      .spike_managed = spike_managed,
  };
}

#define META(name, operand_form, rd_class, rs1_class, rs2_class, rs3_class, imm_kind, uniform_transfer_kind, scalar_exec_kind, spike_managed) \
  meta(name, operand_form, rd_class, rs1_class, rs2_class, rs3_class, imm_kind, uniform_transfer_kind, scalar_exec_kind, spike_managed)

static constexpr auto kInstMetadata = std::to_array<InstMetadata>({
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

bool is_non_classified_control_name(std::string_view name) {
  return name == "barrier" || name == "endprg" || name == "join" || name == "regext" || name == "regexti" || name == "setrpc" ||
         name == "system" || name == "unknown" || name == "vsetvli";
}

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
}

bool populate_inst_metadata(std::string_view name, DecodedInst &out) {
  if (const auto *metadata = find_inst_metadata(name)) {
    apply_inst_metadata(*metadata, out);
    return true;
  }
  return false;
}

bool is_scalar_exec_classification_required(const DecodedInst &di) {
  if (di.custom.valid || di.mma.valid) return false;
  if (is_non_classified_control_name(di.name)) return false;
  if (di.name == "jal" || di.name == "jalr") return false;
  if (di.name == "vmv_x_s") return true;
  const bool has_vector_operand = di.rd_class == RegClass::V || di.rs1_class == RegClass::V || di.rs2_class == RegClass::V || di.rs3_class == RegClass::V;
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
