#pragma once

#include "sbt/emit_descriptor.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sbt {

enum class RegClass : uint8_t {
  None = 0,
  X = 1,
  V = 2,
};

enum class ImmKind : uint8_t {
  None = 0,
  I12,
  S12,
  B13,
  U20,
  J21,
  CSR12,
  UImm5,
  SImm5,
  Raw12,
};

using InstId = uint32_t;

inline constexpr InstId kUnknownInstId = 0;

enum class OperandForm : uint8_t {
  None = 0,
  XRdRs1Imm,
  XRdRs1Rs2,
  XRdRs1Rs2Rs3,
  XRdImm,
  XRdCsrRs1,
  XRdCsrImm,
  XRs1Rs2Imm,
  VRd,
  VRdImm,
  VRdRs1Scalar,
  VRdRs1VectorImm,
  VRdRs2Vector,
  VRdRs2VectorRs1Scalar,
  VRdRs2VectorRs1Vector,
  VRdRs2VectorImm,
  XRdRs2Vector,
  VRs1VectorRs2VectorImm,
};

enum class UniformTransferKind : uint8_t {
  Unknown = 0,
  NotApplicable,
  AlwaysUniformDst,
  NeverUniformDst,
  UniformIfRs1,
  UniformIfRs2,
  UniformIfRs1AndRs2,
};

enum class ScalarExecKind : uint8_t {
  None = 0,
  UniformPure,
  LaneSensitive,
  FixedLaneSensitive,
  ExternallySideEffecting,
};

// RISC-V floating-point rounding mode (rm field).
// - For scalar FP ops in Ventus, the ISA is close to Zfinx: float values are carried as raw f32 bits in X regs.
// - rm=111 (DYN) means "use CSR.frm". We currently do not model frm; see design doc for the chosen policy.
enum class FpRoundingMode : uint8_t {
  None = 0xFF,
  RNE = 0, // round to nearest, ties to even
  RTZ = 1, // round toward zero
  RDN = 2, // round down (toward -inf)
  RUP = 3, // round up (toward +inf)
  RMM = 4, // round to nearest, ties to max magnitude
  Reserved5 = 5,
  Reserved6 = 6,
  DYN = 7, // dynamic (use CSR.frm)
};

enum class CustomFamily : uint8_t {
  None = 0,
  Shuffle,
  Convert,
  PackedArith,
  Sfu,
  Mma, // Reserved for support-custom-mma change ownership.
};

enum class CustomSubOp : uint8_t {
  None = 0,
  ShuffleIdx,
  ShuffleUp,
  ShuffleDown,
  ShuffleBfly,
  CvtF32FromF16,
  CvtF16FromF32,
  CvtF32FromBf16,
  CvtBf16FromF32,
  Add,
  Mul,
  Fma,
  Ex2,
  Lg2,
  Rcp,
  Sqrt,
  Rsqrt,
  Sin,
  Cos,
  Tanh,
  Gelu,
  Silu,
};

enum class CustomDataType : uint8_t {
  None = 0,
  Fp32,
  Fp16,
  Bf16,
  F16x2,
  Bf16x2,
};

enum class MmaShape : uint8_t {
  None = 0,
  M8N8K16,
  M16N8K16,
  M8N16K16,
  M16N16K16,
  M8N8K8,
  M16N8K8,
  M8N16K8,
  M16N16K8,
};

enum class MmaLayout : uint8_t {
  Row = 0,
  Col = 1,
};

enum class MmaAbType : uint8_t {
  None = 0xFF,
  Tf32 = 0,
  Fp16 = 1,
  Bf16 = 2,
};

enum class MmaCdType : uint8_t {
  None = 0xFF,
  Fp16 = 0,
  Fp32 = 1,
};

enum class MmaLoweringClass : uint8_t {
  Unsupported = 0,
  NativeMmaSync,
  NativeWmma,
  CompositeLowering,
};

enum class FirstBatchMmaClass : uint8_t {
  Unsupported = 0,
  CommittedDirectNative,
  CommittedSplitNComposite,
  Deferred,
  Research,
};

struct MmaInstInfo final {
  bool valid = false;
  MmaShape shape = MmaShape::None;
  MmaLayout a_layout = MmaLayout::Row;
  MmaLayout b_layout = MmaLayout::Row;
  MmaAbType ab_type = MmaAbType::None;
  MmaCdType cd_type = MmaCdType::None;
  bool spike_a_column_layout = false;
  bool spike_b_row_layout = false;
  int rd_base = -1;
  int rs1_base = -1;
  int rs2_base = -1;
  uint8_t a_regs_per_thread = 0;
  uint8_t b_regs_per_thread = 0;
  uint8_t c_regs_per_thread = 0;
  bool wide_ab = false;
  MmaLoweringClass lowering_class = MmaLoweringClass::Unsupported;
  FirstBatchMmaClass support_class = FirstBatchMmaClass::Unsupported;
};

struct CustomInstInfo final {
  bool valid = false;
  CustomFamily family = CustomFamily::None;
  CustomSubOp subop = CustomSubOp::None;
  CustomDataType dtype = CustomDataType::None;
  bool vm_bit = false;
  uint8_t funct6 = 0;
  uint8_t funct3 = 0;
};

struct RegextPrefix final {
  bool valid = false;
  bool validi = false; // regexti: extends rs2/rd and immediate
  uint32_t pc = 0;
  uint16_t imm12 = 0; // Raw imm12 of the last-applied prefix in this bundle.
  uint8_t ext_rd = 0;
  uint8_t ext_rs1 = 0;
  uint8_t ext_rs2 = 0;
  uint8_t ext_rs3 = 0;
  uint8_t ext_imm = 0; // 6-bit field (meaningful when validi=true)
  uint8_t prefix_bytes = 0;
};

struct InstMetadata final {
  InstId id = kUnknownInstId;
  const char *name = nullptr;
  OperandForm operand_form = OperandForm::None;
  RegClass rd_class = RegClass::None;
  RegClass rs1_class = RegClass::None;
  RegClass rs2_class = RegClass::None;
  RegClass rs3_class = RegClass::None;
  ImmKind imm_kind = ImmKind::None;
  UniformTransferKind uniform_transfer_kind = UniformTransferKind::Unknown;
  ScalarExecKind scalar_exec_kind = ScalarExecKind::None;
  EmitDescriptor emit{};
  bool spike_managed = false;
};

struct DecodedInst final {
  uint32_t pc = 0;
  uint32_t word = 0;
  std::string name;
  InstId inst_id = kUnknownInstId;
  OperandForm operand_form = OperandForm::None;
  UniformTransferKind uniform_transfer_kind = UniformTransferKind::Unknown;
  ScalarExecKind scalar_exec_kind = ScalarExecKind::None;

  RegClass rd_class = RegClass::None;
  RegClass rs1_class = RegClass::None;
  RegClass rs2_class = RegClass::None;
  RegClass rs3_class = RegClass::None;

  int rd = -1;
  int rs1 = -1;
  int rs2 = -1;
  int rs3 = -1;

  ImmKind imm_kind = ImmKind::None;
  int32_t imm = 0;

  // FP rm field for scalar F-extension instructions (when applicable).
  FpRoundingMode fp_rm = FpRoundingMode::None;

  // Structured metadata for repository-local custom decode path.
  CustomInstInfo custom{};
  MmaInstInfo mma{};
  EmitDescriptor emit{};

  bool had_regext = false;
  RegextPrefix regext{};
};

struct DecodeOptions final {
  bool bundle_regext = true;
  bool require_known = false;
  bool spike_compat_nested_regext = false;
};

struct Pattern final {
  const char *name = nullptr;
  uint32_t match = 0;
  uint32_t mask = 0;
};

std::vector<DecodedInst>
decode_text(const std::vector<uint8_t> &text, uint32_t text_vaddr, const DecodeOptions &opt,
            const std::vector<Pattern> &patterns);

InstId make_inst_id(std::string_view name);
std::span<const InstMetadata> all_inst_metadata();
const InstMetadata *find_inst_metadata(std::string_view name);
const InstMetadata *find_inst_metadata(InstId id);
void apply_inst_metadata(const InstMetadata &metadata, DecodedInst &out);
bool populate_inst_metadata(std::string_view name, DecodedInst &out);
void finalize_emit_descriptor(DecodedInst &out);
bool is_scalar_exec_classification_required(const DecodedInst &di);

const char *to_string(RegClass c);
const char *to_string(ImmKind k);
const char *to_string(OperandForm form);
const char *to_string(UniformTransferKind kind);
const char *to_string(ScalarExecKind kind);
const char *to_string(FpRoundingMode rm);
const char *to_string(CustomFamily f);
const char *to_string(CustomSubOp op);
const char *to_string(CustomDataType t);
const char *to_string(MmaShape s);
const char *to_string(MmaLayout layout);
const char *to_string(MmaAbType t);
const char *to_string(MmaCdType t);
const char *to_string(MmaLoweringClass c);
const char *to_string(FirstBatchMmaClass c);

} // namespace sbt
