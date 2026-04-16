#include "sbt/riscv_decode.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t w) {
  out.push_back(static_cast<uint8_t>(w & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
}

uint32_t make_custom_word(uint32_t opcode, uint32_t funct3, uint32_t funct6, uint32_t vd, uint32_t vs2, uint32_t vs1_or_imm5,
                          bool vm_bit) {
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (vd & 0x1Fu) << 7;
  w |= (funct3 & 0x7u) << 12;
  w |= (vs1_or_imm5 & 0x1Fu) << 15;
  w |= (vs2 & 0x1Fu) << 20;
  w |= (vm_bit ? 1u : 0u) << 25;
  w |= (funct6 & 0x3Fu) << 26;
  return w;
}

uint32_t make_i_word(uint32_t opcode, uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t imm12) {
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (rd & 0x1Fu) << 7;
  w |= (funct3 & 0x7u) << 12;
  w |= (rs1 & 0x1Fu) << 15;
  w |= (imm12 & 0xFFFu) << 20;
  return w;
}

uint32_t make_s_word(uint32_t opcode, uint32_t funct3, uint32_t rs1, uint32_t rs2, uint32_t imm12) {
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (imm12 & 0x1Fu) << 7;
  w |= (funct3 & 0x7u) << 12;
  w |= (rs1 & 0x1Fu) << 15;
  w |= (rs2 & 0x1Fu) << 20;
  w |= ((imm12 >> 5) & 0x7Fu) << 25;
  return w;
}

uint32_t make_j_word(uint32_t opcode, uint32_t rd, int32_t imm21) {
  const uint32_t u = static_cast<uint32_t>(imm21);
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (rd & 0x1Fu) << 7;
  w |= ((u >> 12) & 0xFFu) << 12;
  w |= ((u >> 11) & 0x1u) << 20;
  w |= ((u >> 1) & 0x3FFu) << 21;
  w |= ((u >> 20) & 0x1u) << 31;
  return w;
}

} // namespace

int main() {
  const uint32_t w_shuffle_idx = make_custom_word(/*opcode=*/0x42u, /*funct3=*/0x1u, /*funct6=*/0x09u, /*vd=*/3u, /*vs2=*/4u,
                                                  /*imm5=*/7u, /*vm=*/true);
  const uint32_t w_vcvt_fp32_bf16 =
      make_custom_word(/*opcode=*/0x7Au, /*funct3=*/0x0u, /*funct6=*/0x02u, /*vd=*/5u, /*vs2=*/6u, /*vs1=*/0u, /*vm=*/true);
  const uint32_t w_vadd_bf16x2 =
      make_custom_word(/*opcode=*/0x5Au, /*funct3=*/0x1u, /*funct6=*/0x00u, /*vd=*/7u, /*vs2=*/9u, /*vs1=*/8u, /*vm=*/true);
  const uint32_t w_vsilu_approx_f16x2 =
      make_custom_word(/*opcode=*/0x2Au, /*funct3=*/0x1u, /*funct6=*/0x09u, /*vd=*/10u, /*vs2=*/11u, /*vs1=*/0u, /*vm=*/false);

  std::vector<uint8_t> text;
  append_u32_le(text, w_shuffle_idx);
  append_u32_le(text, w_vcvt_fp32_bf16);
  append_u32_le(text, w_vadd_bf16x2);
  append_u32_le(text, w_vsilu_approx_f16x2);

  sbt::DecodeOptions opt;
  opt.bundle_regext = true;
  opt.require_known = true;
  const std::vector<sbt::Pattern> no_spike_patterns;
  const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x80000000u, opt, no_spike_patterns);

  require(decoded.size() == 4, "all custom non-MMA words decoded without Spike patterns");
  require(decoded[0].name == "shuffle_idx", "shuffle idx name");
  require(decoded[0].custom.valid, "shuffle metadata present");
  require(decoded[0].operand_form == sbt::OperandForm::None, "custom path does not get overwritten by shared Spike metadata");
  require(decoded[0].custom.vm_bit, "shuffle vm bit kept as encoding metadata");
  require(decoded[0].custom.family == sbt::CustomFamily::Shuffle, "shuffle family");
  require(decoded[0].custom.subop == sbt::CustomSubOp::ShuffleIdx, "shuffle subop");
  require(decoded[0].imm == 7, "shuffle imm extracted from imm5 field");

  require(decoded[1].name == "vcvt_fp32_bf16", "vcvt name");
  require(decoded[1].custom.family == sbt::CustomFamily::Convert, "vcvt family");
  require(decoded[1].custom.dtype == sbt::CustomDataType::Bf16, "vcvt dtype");

  require(decoded[2].name == "vadd_bf16x2", "packed add name");
  require(decoded[2].custom.family == sbt::CustomFamily::PackedArith, "packed arith family");
  require(decoded[2].custom.subop == sbt::CustomSubOp::Add, "packed add subop");
  require(decoded[2].rs1 == 8, "packed add keeps vs1");

  require(decoded[3].name == "vsilu_approx_f16x2", "sfu name");
  require(decoded[3].custom.family == sbt::CustomFamily::Sfu, "sfu family");
  require(decoded[3].custom.subop == sbt::CustomSubOp::Silu, "sfu subop");
  require(decoded[3].custom.dtype == sbt::CustomDataType::F16x2, "sfu dtype");

  // Decode pollution guard: scalar instructions must not be tagged as custom.
  std::vector<uint8_t> scalar_text;
  const uint32_t w_addi = make_i_word(/*opcode=*/0x13u, /*funct3=*/0x0u, /*rd=*/1u, /*rs1=*/2u, /*imm12=*/123u);
  const uint32_t w_lw = make_i_word(/*opcode=*/0x03u, /*funct3=*/0x2u, /*rd=*/3u, /*rs1=*/4u, /*imm12=*/16u);
  const uint32_t w_sw = make_s_word(/*opcode=*/0x23u, /*funct3=*/0x2u, /*rs1=*/6u, /*rs2=*/5u, /*imm12=*/20u);
  const uint32_t w_jal = make_j_word(/*opcode=*/0x6Fu, /*rd=*/7u, /*imm21=*/8);
  append_u32_le(scalar_text, w_addi);
  append_u32_le(scalar_text, w_lw);
  append_u32_le(scalar_text, w_sw);
  append_u32_le(scalar_text, w_jal);
  const auto decoded_scalar = sbt::decode_text(scalar_text, /*text_vaddr=*/0xA0000000u, opt, no_spike_patterns);
  require(decoded_scalar.size() == 4, "scalar words decoded");
  require(decoded_scalar[0].name == "addi", "addi decoded");
  require(decoded_scalar[1].name == "lw", "lw decoded");
  require(decoded_scalar[2].name == "sw", "sw decoded");
  require(decoded_scalar[3].name == "jal", "jal decoded");
  require(decoded_scalar[0].inst_id != sbt::kUnknownInstId, "scalar addi gets shared inst id");
  require(decoded_scalar[0].scalar_exec_kind == sbt::ScalarExecKind::UniformPure, "scalar addi gets shared scalar exec metadata");
  require(decoded_scalar[1].scalar_exec_kind == sbt::ScalarExecKind::UniformPure, "scalar load gets shared scalar exec metadata");
  require(decoded_scalar[2].scalar_exec_kind == sbt::ScalarExecKind::ExternallySideEffecting, "scalar store gets explicit side-effect metadata");
  require(!decoded_scalar[0].custom.valid, "addi must not carry fake custom metadata");
  require(!decoded_scalar[1].custom.valid, "lw must not carry fake custom metadata");
  require(!decoded_scalar[2].custom.valid, "sw must not carry fake custom metadata");
  require(!decoded_scalar[3].custom.valid, "jal must not carry fake custom metadata");

  std::cout << "ok custom decode path\n";
  return 0;
}
