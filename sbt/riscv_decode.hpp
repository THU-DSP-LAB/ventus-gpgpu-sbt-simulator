#pragma once

#include <cstdint>
#include <string>
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
  uint16_t imm12 = 0;
  uint8_t ext_rd = 0;
  uint8_t ext_rs1 = 0;
  uint8_t ext_rs2 = 0;
  uint8_t ext_rs3 = 0;
  uint8_t ext_imm = 0; // 6-bit field (meaningful when validi=true)
};

struct DecodedInst final {
  uint32_t pc = 0;
  uint32_t word = 0;
  std::string name;

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

  bool had_regext = false;
  RegextPrefix regext{};
};

struct DecodeOptions final {
  bool bundle_regext = true;
  bool require_known = false;
};

struct Pattern final {
  const char *name = nullptr;
  uint32_t match = 0;
  uint32_t mask = 0;
};

std::vector<DecodedInst>
decode_text(const std::vector<uint8_t> &text, uint32_t text_vaddr, const DecodeOptions &opt,
            const std::vector<Pattern> &patterns);

const char *to_string(RegClass c);
const char *to_string(ImmKind k);
const char *to_string(FpRoundingMode rm);
const char *to_string(CustomFamily f);
const char *to_string(CustomSubOp op);
const char *to_string(CustomDataType t);

} // namespace sbt
