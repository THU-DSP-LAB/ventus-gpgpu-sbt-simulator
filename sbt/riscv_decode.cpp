#include "sbt/riscv_decode.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace sbt {
namespace {

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

static uint32_t read_u32_le(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static int32_t sext(uint32_t x, int bits) {
  const uint32_t m = 1u << (bits - 1);
  const uint32_t mask = (bits == 32) ? 0xFFFF'FFFFu : ((1u << bits) - 1);
  x &= mask;
  return (x ^ m) - int32_t(m);
}

static uint32_t imm_i(uint32_t w) { return (w >> 20) & 0xFFFu; }
static uint32_t imm_s(uint32_t w) { return ((w >> 25) << 5) | ((w >> 7) & 0x1Fu); }
static uint32_t imm_b(uint32_t w) {
  const uint32_t bit12 = (w >> 31) & 1u;
  const uint32_t bit11 = (w >> 7) & 1u;
  const uint32_t bits10_5 = (w >> 25) & 0x3Fu;
  const uint32_t bits4_1 = (w >> 8) & 0xFu;
  return (bit12 << 12) | (bit11 << 11) | (bits10_5 << 5) | (bits4_1 << 1);
}
static uint32_t imm_u(uint32_t w) { return w & 0xFFFFF000u; }
static uint32_t imm_j(uint32_t w) {
  const uint32_t bit20 = (w >> 31) & 1u;
  const uint32_t bits10_1 = (w >> 21) & 0x3FFu;
  const uint32_t bit11 = (w >> 20) & 1u;
  const uint32_t bits19_12 = (w >> 12) & 0xFFu;
  return (bit20 << 20) | (bits19_12 << 12) | (bit11 << 11) | (bits10_1 << 1);
}

static int ext_apply(int base5, uint8_t ext3) { return base5 | (int(ext3) << 5); }

static void start_regext_bundle(RegextPrefix &px, uint32_t pc) {
  constexpr uint8_t kMaxPrefixBytesBeforeAnotherPrefix = static_cast<uint8_t>(0xFFu - 8u);
  if (!px.valid) {
    px = RegextPrefix{};
    px.valid = true;
    px.pc = pc;
    px.prefix_bytes = 4;
    return;
  }
  if (px.prefix_bytes > kMaxPrefixBytesBeforeAnotherPrefix) {
    throw std::runtime_error("regext prefix chain too long at pc=" + hex_u32(pc));
  }
  px.prefix_bytes = static_cast<uint8_t>(px.prefix_bytes + 4);
}

static void apply_regext_fields(RegextPrefix &px, uint16_t imm12) {
  px.valid = true;
  px.imm12 = imm12;
  px.ext_rd = imm12 & 7u;
  px.ext_rs1 = (imm12 >> 3) & 7u;
  px.ext_rs2 = (imm12 >> 6) & 7u;
  px.ext_rs3 = (imm12 >> 9) & 7u;
  px.ext_imm = 0;
}

static void apply_regexti_fields(RegextPrefix &px, uint16_t imm12) {
  px.valid = true;
  px.validi = true;
  px.imm12 = imm12;
  px.ext_rd = imm12 & 7u;
  px.ext_rs1 = 0;
  px.ext_rs2 = (imm12 >> 3) & 7u;
  px.ext_rs3 = 0;
  px.ext_imm = (imm12 >> 6) & 0x3Fu;
}

static Pattern const *match_pattern(uint32_t w, const std::vector<Pattern> &patterns) {
  for (const auto &p : patterns) {
    if (p.name == nullptr) continue;
    if ((w & p.mask) == p.match) return &p;
  }
  return nullptr;
}

static void init_custom_non_mma_common(uint32_t w, DecodedInst &out) {
  out.rd_class = RegClass::V;
  out.rs1_class = RegClass::None;
  out.rs2_class = RegClass::V;
  out.rs3_class = RegClass::None;
  out.imm_kind = ImmKind::None;
  out.imm = 0;
  out.custom.valid = true;
  out.custom.vm_bit = ((w >> 25) & 0x1u) != 0;
  out.custom.funct6 = static_cast<uint8_t>((w >> 26) & 0x3Fu);
  out.custom.funct3 = static_cast<uint8_t>((w >> 12) & 0x7u);
  out.rd = static_cast<int>((w >> 7) & 0x1Fu);
  out.rs2 = static_cast<int>((w >> 20) & 0x1Fu);
}

struct MmaShapeInfo final {
  MmaShape shape = MmaShape::None;
  uint8_t a_regs_per_thread = 0;
  uint8_t b_regs_per_thread = 0;
  uint8_t c_regs_per_thread = 0;
  bool k8_shape = false;
  bool m16_shape = false;
  bool n16_shape = false;
};

static bool decode_mma_shape_info(uint32_t shape_bits, MmaShapeInfo &out) {
  switch (shape_bits) {
  case 0u:
    out = {MmaShape::M8N8K16, 2, 2, 2, false, false, false};
    return true;
  case 1u:
    out = {MmaShape::M16N8K16, 4, 2, 4, false, true, false};
    return true;
  case 2u:
    out = {MmaShape::M8N16K16, 2, 4, 4, false, false, true};
    return true;
  case 3u:
    out = {MmaShape::M16N16K16, 4, 4, 8, false, true, true};
    return true;
  case 4u:
    out = {MmaShape::M8N8K8, 2, 2, 2, true, false, false};
    return true;
  case 5u:
    out = {MmaShape::M16N8K8, 4, 2, 4, true, true, false};
    return true;
  case 6u:
    out = {MmaShape::M8N16K8, 2, 4, 4, true, false, true};
    return true;
  case 7u:
    out = {MmaShape::M16N16K8, 4, 4, 8, true, true, true};
    return true;
  default: return false;
  }
}

static bool is_valid_mma_type(MmaShapeInfo shape, MmaAbType ab_type, MmaCdType cd_type) {
  if (ab_type == MmaAbType::Tf32) return shape.k8_shape && cd_type == MmaCdType::Fp32;
  if (ab_type == MmaAbType::Fp16) return cd_type == MmaCdType::Fp16 || cd_type == MmaCdType::Fp32;
  if (ab_type == MmaAbType::Bf16) return cd_type == MmaCdType::Fp32;
  return false;
}

static std::string make_mma_name(const MmaInstInfo &mma) {
  return std::string("mma_") + to_string(mma.shape) + "_" + to_string(mma.a_layout) + "_" + to_string(mma.b_layout) + "_ab_" +
         to_string(mma.ab_type) + "_cd_" + to_string(mma.cd_type);
}

static bool decode_repo_local_custom_non_mma(uint32_t w, DecodedInst &out) {
  const uint32_t opcode = w & 0x7Fu;
  const uint32_t funct3 = (w >> 12) & 0x7u;
  const uint32_t funct6 = (w >> 26) & 0x3Fu;
  const int imm5_or_vs1 = static_cast<int>((w >> 15) & 0x1Fu);

  // Repository-local decode path for custom non-MMA families.
  if (opcode == 0x42u && funct3 == 0x1u) {
    DecodedInst cand = out;
    init_custom_non_mma_common(w, cand);
    cand.custom.family = CustomFamily::Shuffle;
    cand.imm_kind = ImmKind::UImm5;
    cand.imm = imm5_or_vs1;
    if (funct6 == 0x9u) {
      cand.name = "shuffle_idx";
      cand.custom.subop = CustomSubOp::ShuffleIdx;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0xAu) {
      cand.name = "shuffle_up";
      cand.custom.subop = CustomSubOp::ShuffleUp;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0xBu) {
      cand.name = "shuffle_down";
      cand.custom.subop = CustomSubOp::ShuffleDown;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0x8u) {
      cand.name = "shuffle_bfly";
      cand.custom.subop = CustomSubOp::ShuffleBfly;
      out = std::move(cand);
      return true;
    }
    return false;
  }

  if (opcode == 0x7Au && funct3 == 0x0u) {
    DecodedInst cand = out;
    init_custom_non_mma_common(w, cand);
    cand.custom.family = CustomFamily::Convert;
    if (funct6 == 0x00u) {
      cand.name = "vcvt_f32_fp16";
      cand.custom.subop = CustomSubOp::CvtF32FromF16;
      cand.custom.dtype = CustomDataType::Fp16;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0x01u) {
      cand.name = "vcvt_f16_fp32";
      cand.custom.subop = CustomSubOp::CvtF16FromF32;
      cand.custom.dtype = CustomDataType::Fp16;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0x02u) {
      cand.name = "vcvt_fp32_bf16";
      cand.custom.subop = CustomSubOp::CvtF32FromBf16;
      cand.custom.dtype = CustomDataType::Bf16;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0x03u) {
      cand.name = "vcvt_bf16_fp32";
      cand.custom.subop = CustomSubOp::CvtBf16FromF32;
      cand.custom.dtype = CustomDataType::Bf16;
      out = std::move(cand);
      return true;
    }
    return false;
  }

  if (opcode == 0x5Au) {
    DecodedInst cand = out;
    init_custom_non_mma_common(w, cand);
    cand.custom.family = CustomFamily::PackedArith;
    cand.rs1_class = RegClass::V;
    cand.rs1 = imm5_or_vs1;
    if (funct3 == 0x0u) cand.custom.dtype = CustomDataType::F16x2;
    else if (funct3 == 0x1u) cand.custom.dtype = CustomDataType::Bf16x2;
    else return false;

    if (funct6 == 0x00u) {
      cand.name = (cand.custom.dtype == CustomDataType::F16x2) ? "vadd_f16x2" : "vadd_bf16x2";
      cand.custom.subop = CustomSubOp::Add;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0x01u) {
      cand.name = (cand.custom.dtype == CustomDataType::F16x2) ? "vmul_f16x2" : "vmul_bf16x2";
      cand.custom.subop = CustomSubOp::Mul;
      out = std::move(cand);
      return true;
    }
    if (funct6 == 0x02u) {
      cand.name = (cand.custom.dtype == CustomDataType::F16x2) ? "vfma_f16x2" : "vfma_bf16x2";
      cand.custom.subop = CustomSubOp::Fma;
      out = std::move(cand);
      return true;
    }
    return false;
  }

  if (opcode == 0x2Au) {
    DecodedInst cand = out;
    init_custom_non_mma_common(w, cand);
    cand.custom.family = CustomFamily::Sfu;
    if (funct3 == 0x0u) cand.custom.dtype = CustomDataType::Fp32;
    else if (funct3 == 0x1u) cand.custom.dtype = CustomDataType::F16x2;
    else if (funct3 == 0x2u) cand.custom.dtype = CustomDataType::Bf16x2;
    else return false;

    auto dtype_suffix = [&]() -> std::string_view {
      if (cand.custom.dtype == CustomDataType::Fp32) return "f32";
      if (cand.custom.dtype == CustomDataType::F16x2) return "f16x2";
      return "bf16x2";
    };
    auto set_sfu = [&](std::string_view stem, CustomSubOp subop) {
      cand.custom.subop = subop;
      cand.name = std::string(stem) + "_approx_" + std::string(dtype_suffix());
      out = std::move(cand);
      return true;
    };

    if (cand.custom.dtype == CustomDataType::Fp32) {
      if (funct6 == 0x00u) return set_sfu("vex2", CustomSubOp::Ex2);
      if (funct6 == 0x01u) return set_sfu("vlg2", CustomSubOp::Lg2);
      if (funct6 == 0x02u) return set_sfu("vrcp", CustomSubOp::Rcp);
      if (funct6 == 0x03u) return set_sfu("vsqrt", CustomSubOp::Sqrt);
      if (funct6 == 0x04u) return set_sfu("vrsqrt", CustomSubOp::Rsqrt);
      if (funct6 == 0x05u) return set_sfu("vsin", CustomSubOp::Sin);
      if (funct6 == 0x06u) return set_sfu("vcos", CustomSubOp::Cos);
      if (funct6 == 0x07u) return set_sfu("vtanh", CustomSubOp::Tanh);
      if (funct6 == 0x08u) return set_sfu("vgelu", CustomSubOp::Gelu);
      if (funct6 == 0x09u) return set_sfu("vsilu", CustomSubOp::Silu);
      return false;
    }

    if (funct6 == 0x00u) return set_sfu("vex2", CustomSubOp::Ex2);
    if (funct6 == 0x02u) return set_sfu("vrcp", CustomSubOp::Rcp);
    if (funct6 == 0x03u) return set_sfu("vsqrt", CustomSubOp::Sqrt);
    if (funct6 == 0x04u) return set_sfu("vrsqrt", CustomSubOp::Rsqrt);
    if (funct6 == 0x07u) return set_sfu("vtanh", CustomSubOp::Tanh);
    if (funct6 == 0x08u) return set_sfu("vgelu", CustomSubOp::Gelu);
    if (funct6 == 0x09u) return set_sfu("vsilu", CustomSubOp::Silu);
    return false;
  }

  // Leave MMA ownership to support-custom-mma change.
  return false;
}

static bool decode_repo_local_custom_mma(uint32_t w, DecodedInst &out) {
  if ((w & 0x7Fu) != 0x0Au) return false;

  const uint32_t shape_bits = (w >> 25) & 0x7u;
  const uint32_t abtype_bits = (w >> 28) & 0xFu;
  const uint32_t cdtype_bit = (w >> 12) & 0x1u;
  const bool alayout_bit = ((w >> 14) & 0x1u) != 0;
  const bool blayout_bit = ((w >> 13) & 0x1u) != 0;

  MmaShapeInfo shape{};
  if (!decode_mma_shape_info(shape_bits, shape)) return false;

  MmaAbType ab_type = MmaAbType::None;
  if (abtype_bits == 0u) ab_type = MmaAbType::Tf32;
  else if (abtype_bits == 1u) ab_type = MmaAbType::Fp16;
  else if (abtype_bits == 2u) ab_type = MmaAbType::Bf16;
  else return false;

  const MmaCdType cd_type = (cdtype_bit == 0u) ? MmaCdType::Fp16 : MmaCdType::Fp32;
  if (!is_valid_mma_type(shape, ab_type, cd_type)) return false;

  DecodedInst cand = out;
  cand.rd_class = RegClass::V;
  cand.rs1_class = RegClass::V;
  cand.rs2_class = RegClass::V;
  cand.rs3_class = RegClass::None;
  cand.imm_kind = ImmKind::None;
  cand.imm = 0;
  cand.custom.valid = true;
  cand.custom.family = CustomFamily::Mma;
  cand.custom.funct6 = static_cast<uint8_t>((w >> 26) & 0x3Fu);
  cand.custom.funct3 = static_cast<uint8_t>((w >> 12) & 0x7u);
  cand.rd = static_cast<int>((w >> 7) & 0x1Fu);
  cand.rs1 = static_cast<int>((w >> 15) & 0x1Fu);
  cand.rs2 = static_cast<int>((w >> 20) & 0x1Fu);

  cand.mma.valid = true;
  cand.mma.shape = shape.shape;
  cand.mma.a_layout = alayout_bit ? MmaLayout::Col : MmaLayout::Row;
  cand.mma.b_layout = blayout_bit ? MmaLayout::Row : MmaLayout::Col;
  cand.mma.ab_type = ab_type;
  cand.mma.cd_type = cd_type;
  cand.mma.spike_a_column_layout = alayout_bit;
  cand.mma.spike_b_row_layout = blayout_bit;
  cand.mma.rd_base = cand.rd;
  cand.mma.rs1_base = cand.rs1;
  cand.mma.rs2_base = cand.rs2;
  cand.mma.a_regs_per_thread = shape.a_regs_per_thread;
  cand.mma.b_regs_per_thread = shape.b_regs_per_thread;
  cand.mma.c_regs_per_thread = shape.c_regs_per_thread;
  cand.mma.wide_ab = (ab_type == MmaAbType::Tf32);
  cand.mma.lowering_class = MmaLoweringClass::Unsupported;
  cand.mma.support_class = FirstBatchMmaClass::Unsupported;

  const bool row_col = cand.mma.a_layout == MmaLayout::Row && cand.mma.b_layout == MmaLayout::Col;
  const bool first_batch_shape = shape.m16_shape;
  if (first_batch_shape && row_col) {
    if (shape.n16_shape) {
      cand.mma.support_class = FirstBatchMmaClass::CommittedSplitNComposite;
      cand.mma.lowering_class = MmaLoweringClass::CompositeLowering;
    } else {
      cand.mma.support_class = FirstBatchMmaClass::CommittedDirectNative;
      cand.mma.lowering_class = MmaLoweringClass::NativeMmaSync;
    }
  } else if (first_batch_shape) {
    cand.mma.support_class = FirstBatchMmaClass::Deferred;
  } else {
    cand.mma.support_class = FirstBatchMmaClass::Research;
  }

  cand.name = make_mma_name(cand.mma);
  out = std::move(cand);
  return true;
}

static bool decode_scalar(uint32_t w, DecodedInst &out) {
  const uint32_t opcode = w & 0x7Fu;
  const uint32_t rd5 = (w >> 7) & 0x1Fu;
  const uint32_t funct3 = (w >> 12) & 0x7u;
  const uint32_t rs1_5 = (w >> 15) & 0x1Fu;
  const uint32_t rs2_5 = (w >> 20) & 0x1Fu;
  const uint32_t funct7 = (w >> 25) & 0x7Fu;

  auto decode_fp = [&]() -> bool {
    auto set_fp_r = [&](std::string n, FpRoundingMode rm) {
      out.name = std::move(n);
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.rs2_class = RegClass::X;
      out.fp_rm = rm;
    };
    auto set_fp_r_rm = [&](std::string n) {
      set_fp_r(std::move(n), static_cast<FpRoundingMode>(funct3));
    };
    auto set_fp_r_nrm = [&](std::string n) {
      set_fp_r(std::move(n), FpRoundingMode::None);
    };

    // Scalar FP loads/stores (Zfinx: use X regs for f32 bits).
    if (opcode == 0x07u) { // LOAD-FP
      if (funct3 != 0x2u) return false; // only flw
      out.name = "flw";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.imm_kind = ImmKind::I12;
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      out.imm = sext(imm_i(w), 12);
      return true;
    }
    if (opcode == 0x27u) { // STORE-FP
      if (funct3 != 0x2u) return false; // only fsw
      out.name = "fsw";
      out.rs1_class = RegClass::X;
      out.rs2_class = RegClass::X;
      out.imm_kind = ImmKind::S12;
      out.rs1 = int(rs1_5);
      out.rs2 = int(rs2_5);
      out.imm = sext(imm_s(w), 12);
      return true;
    }

    // Scalar FP fused multiply-add family (R4-type).
    if (opcode == 0x43u || opcode == 0x47u || opcode == 0x4Bu || opcode == 0x4Fu) {
      const uint32_t fmt2 = (w >> 25) & 0x3u; // bits [26:25]
      if (fmt2 != 0x0u) return false;         // only .s
      const uint32_t rs3_5 = (w >> 27) & 0x1Fu;

      if (opcode == 0x43u) set_fp_r_rm("fmadd_s");
      else if (opcode == 0x47u) set_fp_r_rm("fmsub_s");
      else if (opcode == 0x4Bu) set_fp_r_rm("fnmsub_s");
      else if (opcode == 0x4Fu) set_fp_r_rm("fnmadd_s");
      else return false;

      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      out.rs2 = int(rs2_5);
      out.rs3_class = RegClass::X;
      out.rs3 = int(rs3_5);
      return true;
    }

    // Scalar FP ops / conversions.
    if (opcode != 0x53u) return false;

    // Arithmetic (rm in funct3).
    if (funct7 == 0x00u) { set_fp_r_rm("fadd_s"); }
    else if (funct7 == 0x04u) { set_fp_r_rm("fsub_s"); }
    else if (funct7 == 0x08u) { set_fp_r_rm("fmul_s"); }
    else if (funct7 == 0x0Cu) { set_fp_r_rm("fdiv_s"); }
    else if (funct7 == 0x2Cu && rs2_5 == 0x0u) { set_fp_r_rm("fsqrt_s"); }
    // Sign injection (funct3 is funct3, not rm).
    else if (funct7 == 0x10u && funct3 == 0x0u) { set_fp_r_nrm("fsgnj_s"); }
    else if (funct7 == 0x10u && funct3 == 0x1u) { set_fp_r_nrm("fsgnjn_s"); }
    else if (funct7 == 0x10u && funct3 == 0x2u) { set_fp_r_nrm("fsgnjx_s"); }
    // Min/max (funct3 is funct3, not rm).
    else if (funct7 == 0x14u && funct3 == 0x0u) { set_fp_r_nrm("fmin_s"); }
    else if (funct7 == 0x14u && funct3 == 0x1u) { set_fp_r_nrm("fmax_s"); }
    // Float compare (funct3 selects op, not rm): rd is integer.
    else if (funct7 == 0x50u && funct3 == 0x2u) { set_fp_r_nrm("feq_s"); }
    else if (funct7 == 0x50u && funct3 == 0x1u) { set_fp_r_nrm("flt_s"); }
    else if (funct7 == 0x50u && funct3 == 0x0u) { set_fp_r_nrm("fle_s"); }
    // Float-to-int conversions (rm in funct3, rs2 encodes int type).
    else if (funct7 == 0x60u && rs2_5 == 0x0u) { // fcvt.w.s
      out.name = "fcvt_w_s";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.fp_rm = static_cast<FpRoundingMode>(funct3);
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    } else if (funct7 == 0x60u && rs2_5 == 0x1u) { // fcvt.wu.s
      out.name = "fcvt_wu_s";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.fp_rm = static_cast<FpRoundingMode>(funct3);
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    }
    // Int-to-float conversions (rm in funct3, rs2 encodes int type).
    else if (funct7 == 0x68u && rs2_5 == 0x0u) { // fcvt.s.w
      out.name = "fcvt_s_w";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.fp_rm = static_cast<FpRoundingMode>(funct3);
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    } else if (funct7 == 0x68u && rs2_5 == 0x1u) { // fcvt.s.wu
      out.name = "fcvt_s_wu";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.fp_rm = static_cast<FpRoundingMode>(funct3);
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    }
    // fmv.x.w / fclass.s (rs2==0).
    else if (funct7 == 0x70u && rs2_5 == 0x0u && funct3 == 0x0u) { // fmv.x.w
      out.name = "fmv_x_w";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    } else if (funct7 == 0x70u && rs2_5 == 0x0u && funct3 == 0x1u) { // fclass.s
      out.name = "fclass_s";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    }
    // fmv.w.x (rs2==0, rm=0 in spec).
    else if (funct7 == 0x78u && rs2_5 == 0x0u && funct3 == 0x0u) {
      out.name = "fmv_w_x";
      out.rd_class = RegClass::X;
      out.rs1_class = RegClass::X;
      out.rd = int(rd5);
      out.rs1 = int(rs1_5);
      return true;
    } else {
      return false;
    }

    // Shared register fill for most opcode=0x53 R-type ops.
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    return true;
  };

  if (decode_fp()) return true;

  auto set_r = [&](std::string n) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
  };
  auto set_i = [&](std::string n, ImmKind k = ImmKind::I12) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.imm_kind = k;
  };
  auto set_s = [&](std::string n) {
    out.name = std::move(n);
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
    out.imm_kind = ImmKind::S12;
  };

  auto set_b = [&](std::string n) {
    out.name = std::move(n);
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
    out.imm_kind = ImmKind::B13;
  };
  auto set_u = [&](std::string n) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.imm_kind = ImmKind::U20;
  };
  auto set_j = [&](std::string n) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.imm_kind = ImmKind::J21;
  };

  switch (opcode) {
  case 0x37: // LUI
    set_u("lui");
    out.rd = int(rd5);
    out.imm = int32_t(imm_u(w));
    return true;
  case 0x17: // AUIPC
    set_u("auipc");
    out.rd = int(rd5);
    out.imm = int32_t(imm_u(w));
    return true;
  case 0x6F: // JAL
    set_j("jal");
    out.rd = int(rd5);
    out.imm = sext(imm_j(w), 21);
    return true;
  case 0x67: // JALR
    if (funct3 != 0) return false;
    set_i("jalr");
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.imm = sext(imm_i(w), 12);
    return true;
  case 0x63: // BRANCH
    switch (funct3) {
    case 0x0: set_b("beq"); break;
    case 0x1: set_b("bne"); break;
    case 0x4: set_b("blt"); break;
    case 0x5: set_b("bge"); break;
    case 0x6: set_b("bltu"); break;
    case 0x7: set_b("bgeu"); break;
    default: return false;
    }
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    out.imm = sext(imm_b(w), 13);
    return true;
  case 0x03: // LOAD
    switch (funct3) {
    case 0x0: set_i("lb"); break;
    case 0x1: set_i("lh"); break;
    case 0x2: set_i("lw"); break;
    case 0x4: set_i("lbu"); break;
    case 0x5: set_i("lhu"); break;
    default: return false;
    }
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.imm = sext(imm_i(w), 12);
    return true;
  case 0x23: // STORE
    switch (funct3) {
    case 0x0: set_s("sb"); break;
    case 0x1: set_s("sh"); break;
    case 0x2: set_s("sw"); break;
    default: return false;
    }
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    out.imm = sext(imm_s(w), 12);
    return true;
  case 0x13: // OP-IMM
    switch (funct3) {
    case 0x0: set_i("addi"); out.imm = sext(imm_i(w), 12); break;
    case 0x2: set_i("slti"); out.imm = sext(imm_i(w), 12); break;
    case 0x3: set_i("sltiu"); out.imm = sext(imm_i(w), 12); break;
    case 0x4: set_i("xori"); out.imm = sext(imm_i(w), 12); break;
    case 0x6: set_i("ori"); out.imm = sext(imm_i(w), 12); break;
    case 0x7: set_i("andi"); out.imm = sext(imm_i(w), 12); break;
    case 0x1: // SLLI
      if (funct7 != 0x00) return false;
      set_i("slli");
      out.imm = int32_t((w >> 20) & 0x1Fu);
      break;
    case 0x5: // SRLI/SRAI
      if (funct7 == 0x00) {
        set_i("srli");
      } else if (funct7 == 0x20) {
        set_i("srai");
      } else {
        return false;
      }
      out.imm = int32_t((w >> 20) & 0x1Fu);
      break;
    default: return false;
    }
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    return true;
  case 0x33: // OP
    if (funct7 == 0x01) { // M extension
      switch (funct3) {
      case 0x0: set_r("mul"); break;
      case 0x1: set_r("mulh"); break;
      case 0x2: set_r("mulhsu"); break;
      case 0x3: set_r("mulhu"); break;
      case 0x4: set_r("div"); break;
      case 0x5: set_r("divu"); break;
      case 0x6: set_r("rem"); break;
      case 0x7: set_r("remu"); break;
      default: return false;
      }
    } else {
      switch (funct3) {
      case 0x0:
        if (funct7 == 0x00) set_r("add");
        else if (funct7 == 0x20) set_r("sub");
        else return false;
        break;
      case 0x1: set_r("sll"); break;
      case 0x2: set_r("slt"); break;
      case 0x3: set_r("sltu"); break;
      case 0x4: set_r("xor"); break;
      case 0x5:
        if (funct7 == 0x00) set_r("srl");
        else if (funct7 == 0x20) set_r("sra");
        else return false;
        break;
      case 0x6: set_r("or"); break;
      case 0x7: set_r("and"); break;
      default: return false;
      }
    }
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    return true;
  case 0x73: // SYSTEM (CSR)
    if (funct3 == 0) {
      // ecall/ebreak/... ignored in prototype
      out.name = "system";
      return true;
    }
    out.rd_class = RegClass::X;
    out.rs1_class = (funct3 >= 0x5 ? RegClass::None : RegClass::X);
    out.imm_kind = ImmKind::CSR12;
    out.imm = int32_t((w >> 20) & 0xFFFu);
    switch (funct3) {
    case 0x1: out.name = "csrrw"; break;
    case 0x2: out.name = "csrrs"; break;
    case 0x3: out.name = "csrrc"; break;
    case 0x5: out.name = "csrrwi"; break;
    case 0x6: out.name = "csrrsi"; break;
    case 0x7: out.name = "csrrci"; break;
    default: out.name = "csr"; break;
    }
    out.rd = int(rd5);
    if (out.rs1_class == RegClass::X) out.rs1 = int(rs1_5);
    return true;
  default: return false;
  }
}

static DecodedInst decode_one(uint32_t pc, uint32_t w, const std::vector<Pattern> &patterns, const RegextPrefix &px) {
  DecodedInst out;
  out.pc = pc;
  out.word = w;
  out.name = "unknown";

  // Default raw fields
  const int rd5 = int((w >> 7) & 0x1F);
  const int rs1_5 = int((w >> 15) & 0x1F);
  const int rs2_5 = int((w >> 20) & 0x1F);
  const int rs3_5 = int((w >> 27) & 0x1F);

  if (decode_repo_local_custom_non_mma(w, out) || decode_repo_local_custom_mma(w, out)) {
    // Decoded by repository-local custom path.
  } else if (const Pattern *p = match_pattern(w, patterns)) {
    out.name = p->name;
    if (!populate_inst_metadata(out.name, out)) {
      throw std::runtime_error("missing shared instruction metadata for Spike-backed instruction '" + out.name +
                               "'; update data/spike_want.txt and rebuild the pattern subset if this is a newly supported instruction");
    }
    // Fill operands/imm based on classification.
    if (out.rd_class != RegClass::None) out.rd = rd5;
    if (out.rs1_class != RegClass::None) out.rs1 = rs1_5;
    if (out.rs2_class != RegClass::None) out.rs2 = rs2_5;
    if (out.rs3_class != RegClass::None) out.rs3 = rs3_5;

    switch (out.imm_kind) {
    case ImmKind::I12:
      // Some Ventus custom ops reuse I-type encoding but with a non-standard immediate width.
      // `vlw.v` encodes an 11-bit signed offset in bits [30:20] (llvm-objdump prints e.g. 0x7FC as -4).
      if (out.name == "vlw_v") out.imm = sext((w >> 20) & 0x7FFu, 11);
      else out.imm = sext(imm_i(w), 12);
      break;
    case ImmKind::S12:
      // PDS stores encode an 11-bit signed offset using S-type split immediate bits [30:25] and [11:7].
      if (out.name == "vsb_v" || out.name == "vsw_v") {
        const uint32_t hi6 = (w >> 25) & 0x3Fu;   // bits [30:25]
        const uint32_t lo5 = (w >> 7) & 0x1Fu;    // bits [11:7]
        const uint32_t imm11 = (hi6 << 5) | lo5;  // bits [10:0]
        out.imm = sext(imm11, 11);
      } else {
        out.imm = sext(imm_s(w), 12);
      }
      break;
    case ImmKind::B13: out.imm = sext(imm_b(w), 13); break;
    case ImmKind::U20: out.imm = int32_t(imm_u(w)); break;
    case ImmKind::J21: out.imm = sext(imm_j(w), 21); break;
    case ImmKind::CSR12: out.imm = int32_t((w >> 20) & 0xFFFu); break;
    case ImmKind::Raw12: out.imm = int32_t((w >> 20) & 0xFFFu); break;
    case ImmKind::UImm5: out.imm = int32_t((w >> 15) & 0x1Fu); break;
    case ImmKind::SImm5: out.imm = sext((w >> 15) & 0x1Fu, 5); break;
    case ImmKind::None: default: break;
    }
  } else {
    const bool decoded_scalar = decode_scalar(w, out);
    if (decoded_scalar && out.name != "unknown" && out.name != "system" && !populate_inst_metadata(out.name, out)) {
      throw std::runtime_error("missing shared instruction metadata for scalar instruction '" + out.name + "'");
    }
  }

  // Apply regext prefix if present.
  if (px.valid) {
    out.had_regext = true;
    out.regext = px;
    if (out.rd_class != RegClass::None && out.rd >= 0) out.rd = ext_apply(out.rd, px.ext_rd);
    if (out.rs1_class != RegClass::None && out.rs1 >= 0) out.rs1 = ext_apply(out.rs1, px.ext_rs1);
    if (out.rs2_class != RegClass::None && out.rs2 >= 0) out.rs2 = ext_apply(out.rs2, px.ext_rs2);
    if (out.rs3_class != RegClass::None && out.rs3 >= 0) out.rs3 = ext_apply(out.rs3, px.ext_rs3);

    // regexti extends 5-bit immediates (primarily vector *_vi forms).
    if (px.validi && (out.imm_kind == ImmKind::UImm5 || out.imm_kind == ImmKind::SImm5)) {
      const int32_t low5 = int32_t((w >> 15) & 0x1Fu);
      const int32_t ext6 = sext(px.ext_imm & 0x3Fu, 6);
      const int32_t imm11 = (ext6 << 5) + low5;
      out.imm = imm11;
    }
  }

  if (out.mma.valid) {
    out.mma.rd_base = out.rd;
    out.mma.rs1_base = out.rs1;
    out.mma.rs2_base = out.rs2;
  }
  finalize_emit_descriptor(out);
  if (out.inst_id == kUnknownInstId && out.name != "unknown") out.inst_id = make_inst_id(out.name);

  return out;
}

} // namespace

std::vector<DecodedInst> decode_text(const std::vector<uint8_t> &text, uint32_t text_vaddr, const DecodeOptions &opt,
                                     const std::vector<Pattern> &patterns_in) {
  if (text.size() % 4 != 0) throw std::runtime_error(".text size is not multiple of 4");

  std::vector<Pattern> patterns = patterns_in;
  std::sort(patterns.begin(), patterns.end(), [](const Pattern &a, const Pattern &b) {
    const int pa = std::popcount(a.mask);
    const int pb = std::popcount(b.mask);
    if (pa != pb) return pa > pb;
    return std::string_view(a.name) < std::string_view(b.name);
  });

  std::vector<DecodedInst> out;
  out.reserve(text.size() / 4);

  RegextPrefix px{};

  for (size_t off = 0; off < text.size(); off += 4) {
    const uint32_t pc = text_vaddr + static_cast<uint32_t>(off);
    const uint32_t w = read_u32_le(text.data() + off);

    // Handle regext prefix.
    if (opt.bundle_regext) {
      const Pattern *p = match_pattern(w, patterns);
      if (p && std::string_view(p->name) == "regext") {
        const uint16_t imm12 = static_cast<uint16_t>((w >> 20) & 0xFFFu);
        if (px.valid && !opt.spike_compat_nested_regext) {
          throw std::runtime_error("nested regext prefix at pc=" + hex_u32(pc));
        }
        // Temporary compatibility path: when enabled, follow Spike's existing behavior
        // for chained regext/regexti prefixes instead of rejecting nested prefixes.
        start_regext_bundle(px, pc);
        apply_regext_fields(px, imm12);
        continue;
      }
      if (p && std::string_view(p->name) == "regexti") {
        const uint16_t imm12 = static_cast<uint16_t>((w >> 20) & 0xFFFu);
        if (px.valid && !opt.spike_compat_nested_regext) {
          throw std::runtime_error("nested regext prefix at pc=" + hex_u32(pc));
        }
        start_regext_bundle(px, pc);
        apply_regexti_fields(px, imm12);
        continue;
      }
    }

    DecodedInst di = decode_one(pc, w, patterns, px);
    if (opt.require_known && di.name == "unknown") {
      throw std::runtime_error("unknown instruction at pc=" + hex_u32(pc));
    }
    out.push_back(std::move(di));
    px = RegextPrefix{};
  }

  if (px.valid) {
    throw std::runtime_error("dangling regext prefix at pc=" + hex_u32(px.pc));
  }

  return out;
}

const char *to_string(RegClass c) {
  switch (c) {
  case RegClass::X: return "x";
  case RegClass::V: return "v";
  case RegClass::None: default: return "none";
  }
}

const char *to_string(ImmKind k) {
  switch (k) {
  case ImmKind::I12: return "i12";
  case ImmKind::S12: return "s12";
  case ImmKind::B13: return "b13";
  case ImmKind::U20: return "u20";
  case ImmKind::J21: return "j21";
  case ImmKind::CSR12: return "csr12";
  case ImmKind::UImm5: return "uimm5";
  case ImmKind::SImm5: return "simm5";
  case ImmKind::Raw12: return "raw12";
  case ImmKind::None: default: return "none";
  }
}

const char *to_string(FpRoundingMode rm) {
  switch (rm) {
  case FpRoundingMode::RNE: return "rne";
  case FpRoundingMode::RTZ: return "rtz";
  case FpRoundingMode::RDN: return "rdn";
  case FpRoundingMode::RUP: return "rup";
  case FpRoundingMode::RMM: return "rmm";
  case FpRoundingMode::Reserved5: return "reserved5";
  case FpRoundingMode::Reserved6: return "reserved6";
  case FpRoundingMode::DYN: return "dyn";
  case FpRoundingMode::None: default: return "none";
  }
}

const char *to_string(CustomFamily f) {
  switch (f) {
  case CustomFamily::Shuffle: return "shuffle";
  case CustomFamily::Convert: return "convert";
  case CustomFamily::PackedArith: return "packed_arith";
  case CustomFamily::Sfu: return "sfu";
  case CustomFamily::Mma: return "mma";
  case CustomFamily::None: default: return "none";
  }
}

const char *to_string(CustomSubOp op) {
  switch (op) {
  case CustomSubOp::ShuffleIdx: return "shuffle_idx";
  case CustomSubOp::ShuffleUp: return "shuffle_up";
  case CustomSubOp::ShuffleDown: return "shuffle_down";
  case CustomSubOp::ShuffleBfly: return "shuffle_bfly";
  case CustomSubOp::CvtF32FromF16: return "cvt_f32_from_f16";
  case CustomSubOp::CvtF16FromF32: return "cvt_f16_from_f32";
  case CustomSubOp::CvtF32FromBf16: return "cvt_f32_from_bf16";
  case CustomSubOp::CvtBf16FromF32: return "cvt_bf16_from_f32";
  case CustomSubOp::Add: return "add";
  case CustomSubOp::Mul: return "mul";
  case CustomSubOp::Fma: return "fma";
  case CustomSubOp::Ex2: return "ex2";
  case CustomSubOp::Lg2: return "lg2";
  case CustomSubOp::Rcp: return "rcp";
  case CustomSubOp::Sqrt: return "sqrt";
  case CustomSubOp::Rsqrt: return "rsqrt";
  case CustomSubOp::Sin: return "sin";
  case CustomSubOp::Cos: return "cos";
  case CustomSubOp::Tanh: return "tanh";
  case CustomSubOp::Gelu: return "gelu";
  case CustomSubOp::Silu: return "silu";
  case CustomSubOp::None: default: return "none";
  }
}

const char *to_string(CustomDataType t) {
  switch (t) {
  case CustomDataType::Fp32: return "fp32";
  case CustomDataType::Fp16: return "fp16";
  case CustomDataType::Bf16: return "bf16";
  case CustomDataType::F16x2: return "f16x2";
  case CustomDataType::Bf16x2: return "bf16x2";
  case CustomDataType::None: default: return "none";
  }
}

const char *to_string(MmaShape s) {
  switch (s) {
  case MmaShape::M8N8K16: return "m8n8k16";
  case MmaShape::M16N8K16: return "m16n8k16";
  case MmaShape::M8N16K16: return "m8n16k16";
  case MmaShape::M16N16K16: return "m16n16k16";
  case MmaShape::M8N8K8: return "m8n8k8";
  case MmaShape::M16N8K8: return "m16n8k8";
  case MmaShape::M8N16K8: return "m8n16k8";
  case MmaShape::M16N16K8: return "m16n16k8";
  case MmaShape::None: default: return "none";
  }
}

const char *to_string(MmaLayout layout) {
  switch (layout) {
  case MmaLayout::Row: return "row";
  case MmaLayout::Col: return "col";
  default: return "row";
  }
}

const char *to_string(MmaAbType t) {
  switch (t) {
  case MmaAbType::Tf32: return "tf32";
  case MmaAbType::Fp16: return "f16";
  case MmaAbType::Bf16: return "bf16";
  case MmaAbType::None: default: return "none";
  }
}

const char *to_string(MmaCdType t) {
  switch (t) {
  case MmaCdType::Fp16: return "f16";
  case MmaCdType::Fp32: return "f32";
  case MmaCdType::None: default: return "none";
  }
}

const char *to_string(MmaLoweringClass c) {
  switch (c) {
  case MmaLoweringClass::NativeMmaSync: return "native-mma-sync";
  case MmaLoweringClass::NativeWmma: return "native-wmma";
  case MmaLoweringClass::CompositeLowering: return "composite-lowering";
  case MmaLoweringClass::Unsupported: default: return "unsupported";
  }
}

const char *to_string(FirstBatchMmaClass c) {
  switch (c) {
  case FirstBatchMmaClass::CommittedDirectNative: return "committed-direct-native";
  case FirstBatchMmaClass::CommittedSplitNComposite: return "committed-split-n-composite";
  case FirstBatchMmaClass::Deferred: return "deferred";
  case FirstBatchMmaClass::Research: return "research";
  case FirstBatchMmaClass::Unsupported: default: return "unsupported";
  }
}

} // namespace sbt
