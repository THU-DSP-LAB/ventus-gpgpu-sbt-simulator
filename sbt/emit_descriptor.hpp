#pragma once

#include <cstdint>

namespace sbt {

enum class EmitDomain : uint8_t {
  None = 0,
  StructuredControl,
  Control,
  ScalarMemory,
  ScalarInteger,
  ScalarFp,
  Csr,
  VectorMemory,
  VectorRegister,
  VectorInteger,
  VectorCompare,
  VectorMask,
  VectorConvert,
  VectorFp,
  Custom,
  Mma,
};

enum class ExecClassificationApplicability : uint8_t {
  Unknown = 0,
  NotRequired,
  Required,
};

enum class ControlKind : uint8_t {
  None = 0,
  ScalarBranch,
  VectorBranch,
  DirectJump,
  DirectCall,
  Return,
  IndirectTerminator,
};

enum class BranchCondKind : uint8_t {
  None = 0,
  Eq,
  Ne,
  Lt,
  Ge,
  Ltu,
  Geu,
};

enum class StructuredControlKind : uint8_t {
  None = 0,
  SetRpc,
  Join,
  Barrier,
  Vsetvli,
  EndPrg,
};

enum class MemAccessKind : uint8_t {
  None = 0,
  Load,
  Store,
};

enum class MemWidth : uint8_t {
  None = 0,
  Byte = 1,
  Half = 2,
  Word = 4,
};

enum class MemValueKind : uint8_t {
  None = 0,
  Integer,
  FloatBits,
};

enum class MemExtKind : uint8_t {
  None = 0,
  SignExtend,
  ZeroExtend,
  RawBits,
};

enum class MemoryAddrKind : uint8_t {
  None = 0,
  Ordinary,
  Pds,
};

enum class ScalarIntKind : uint8_t {
  None = 0,
  Add,
  Sub,
  And,
  Or,
  Xor,
  Mul,
  MulH,
  MulHU,
  MulHSU,
  Div,
  DivU,
  Rem,
  RemU,
  Lui,
  Auipc,
  ShiftLeft,
  ShiftRightLogical,
  ShiftRightArithmetic,
  SetLessThan,
  SetLessThanU,
};

enum class ScalarFpKind : uint8_t {
  None = 0,
  MoveBits,
  SignInject,
  Binary,
  Sqrt,
  Fma,
  MinMax,
  Compare,
  Convert,
  Classify,
};

enum class FpBinaryKind : uint8_t {
  None = 0,
  Add,
  Sub,
  Mul,
  Div,
};

enum class FpSignInjectKind : uint8_t {
  None = 0,
  CopySign,
  NegateSign,
  XorSign,
};

enum class FpTernaryKind : uint8_t {
  None = 0,
  MAdd,
  MSub,
  NMSub,
  NMAdd,
};

enum class FpMinMaxKind : uint8_t {
  None = 0,
  Min,
  Max,
};

enum class FpCompareKind : uint8_t {
  None = 0,
  Eq,
  Lt,
  Le,
};

enum class FpConvertKind : uint8_t {
  None = 0,
  IntToFloatSigned,
  IntToFloatUnsigned,
  FloatToIntSigned,
  FloatToIntUnsigned,
};

enum class CsrOpKind : uint8_t {
  None = 0,
  Rw,
  Rs,
  Rc,
  Rwi,
  Rsi,
  Rci,
};

enum class CsrSemanticClass : uint8_t {
  None = 0,
  VentusReadonly,
};

enum class VectorRegisterKind : uint8_t {
  None = 0,
  BroadcastScalar,
  InsertScalar,
  BroadcastImmediate,
  MoveVector,
  ExtractScalar,
  LaneId,
  Merge,
};

enum class MergeKind : uint8_t {
  None = 0,
  Vvm,
  Vxm,
  Vim,
  Vfm,
};

enum class VectorIntKind : uint8_t {
  None = 0,
  Add,
  Sub,
  RSub,
  Min,
  MinU,
  Max,
  MaxU,
  And,
  Or,
  Xor,
  ShiftLeft,
  ShiftRightLogical,
  ShiftRightArithmetic,
  Mul,
  MulH,
  MulHU,
  MulHSU,
  Div,
  DivU,
  Rem,
  RemU,
  Madd,
  NMSub,
  MAcc,
  NMSac,
};

enum class VectorCompareKind : uint8_t {
  None = 0,
  Eq,
  Ne,
  Lt,
  LtU,
  Le,
  LeU,
  Gt,
  GtU,
  FEq,
  FLe,
  FLt,
  FGt,
  FGe,
  FNe,
};

enum class VectorMaskKind : uint8_t {
  None = 0,
  And,
  AndNot,
  Or,
  OrNot,
  Xor,
  XNor,
  Nand,
  Nor,
};

enum class VectorConvertKind : uint8_t {
  None = 0,
  FloatFromInt,
  FloatFromUInt,
  IntFromFloat,
  UIntFromFloat,
  IntFromFloatRtz,
  UIntFromFloatRtz,
  Classify,
};

enum class VectorFpKind : uint8_t {
  None = 0,
  Exp,
  Add,
  Sub,
  Mul,
  Div,
  RSub,
  RDiv,
  Min,
  Max,
  MAdd,
  MSub,
  NMAdd,
  NMSub,
  MAcc,
  NMAcc,
  MSac,
  NMSac,
  Sqrt,
  SignInject,
};

enum class VectorFpSignInjectKind : uint8_t {
  None = 0,
  CopySign,
  NegateSign,
  XorSign,
};

struct EmitDescriptor final {
  EmitDomain domain = EmitDomain::None;
  ExecClassificationApplicability scalar_exec_applicability = ExecClassificationApplicability::Unknown;

  ControlKind control_kind = ControlKind::None;
  BranchCondKind branch_cond = BranchCondKind::None;
  StructuredControlKind structured_control_kind = StructuredControlKind::None;

  MemAccessKind mem_access_kind = MemAccessKind::None;
  MemWidth mem_width = MemWidth::None;
  MemValueKind mem_value_kind = MemValueKind::None;
  MemExtKind mem_ext_kind = MemExtKind::None;
  MemoryAddrKind memory_addr_kind = MemoryAddrKind::None;

  ScalarIntKind scalar_int_kind = ScalarIntKind::None;

  ScalarFpKind scalar_fp_kind = ScalarFpKind::None;
  FpBinaryKind fp_binary_kind = FpBinaryKind::None;
  FpSignInjectKind fp_sign_inject_kind = FpSignInjectKind::None;
  FpTernaryKind fp_ternary_kind = FpTernaryKind::None;
  FpMinMaxKind fp_minmax_kind = FpMinMaxKind::None;
  FpCompareKind fp_compare_kind = FpCompareKind::None;
  FpConvertKind fp_convert_kind = FpConvertKind::None;

  CsrOpKind csr_op_kind = CsrOpKind::None;
  CsrSemanticClass csr_semantic_class = CsrSemanticClass::None;

  VectorRegisterKind vector_register_kind = VectorRegisterKind::None;
  MergeKind merge_kind = MergeKind::None;
  VectorIntKind vector_int_kind = VectorIntKind::None;
  VectorCompareKind vector_compare_kind = VectorCompareKind::None;
  VectorMaskKind vector_mask_kind = VectorMaskKind::None;
  VectorConvertKind vector_convert_kind = VectorConvertKind::None;
  VectorFpKind vector_fp_kind = VectorFpKind::None;
  VectorFpSignInjectKind vector_fp_sign_inject_kind = VectorFpSignInjectKind::None;
};

} // namespace sbt
