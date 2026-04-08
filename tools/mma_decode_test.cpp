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

uint32_t make_mma_word(uint32_t abtype, uint32_t shape, bool alayout, bool blayout, uint32_t cdtype, uint32_t vd, uint32_t vs1, uint32_t vs2) {
  uint32_t w = 0;
  w |= (0x0Au & 0x7Fu);
  w |= (vd & 0x1Fu) << 7;
  w |= (cdtype & 0x1u) << 12;
  w |= (alayout ? 1u : 0u) << 14;
  w |= (blayout ? 1u : 0u) << 13;
  w |= (vs1 & 0x1Fu) << 15;
  w |= (vs2 & 0x1Fu) << 20;
  w |= (shape & 0x7u) << 25;
  w |= (abtype & 0xFu) << 28;
  return w;
}

sbt::DecodedInst decode_one_word(uint32_t w) {
  std::vector<uint8_t> text;
  append_u32_le(text, w);
  sbt::DecodeOptions opt;
  opt.bundle_regext = true;
  opt.require_known = true;
  const std::vector<sbt::Pattern> no_spike_patterns;
  const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x90000000u, opt, no_spike_patterns);
  require(decoded.size() == 1, "expect single decoded instruction");
  return decoded[0];
}

bool decode_should_throw(uint32_t w) {
  std::vector<uint8_t> text;
  append_u32_le(text, w);
  sbt::DecodeOptions opt;
  opt.bundle_regext = true;
  opt.require_known = true;
  const std::vector<sbt::Pattern> no_spike_patterns;
  try {
    (void)sbt::decode_text(text, /*text_vaddr=*/0xA0000000u, opt, no_spike_patterns);
    return false;
  } catch (const std::exception &) {
    return true;
  }
}

} // namespace

int main() {
  {
    const auto di = decode_one_word(make_mma_word(
        /*abtype=*/1u, /*shape=*/1u, /*alayout=row=*/false, /*blayout=col=*/false, /*cdtype=*/1u, /*vd=*/3u, /*vs1=*/4u, /*vs2=*/5u));
    require(di.custom.valid, "mma custom metadata valid");
    require(di.custom.family == sbt::CustomFamily::Mma, "mma custom family tag");
    require(di.mma.valid, "mma metadata valid");
    require(di.mma.shape == sbt::MmaShape::M16N8K16, "shape m16n8k16");
    require(di.mma.a_layout == sbt::MmaLayout::Row, "a row");
    require(di.mma.b_layout == sbt::MmaLayout::Col, "b col");
    require(di.mma.ab_type == sbt::MmaAbType::Fp16, "ab fp16");
    require(di.mma.cd_type == sbt::MmaCdType::Fp32, "cd fp32");
    require(di.mma.a_regs_per_thread == 4, "a regs");
    require(di.mma.b_regs_per_thread == 2, "b regs");
    require(di.mma.c_regs_per_thread == 4, "c regs");
    require(!di.mma.wide_ab, "fp16 is not wide ab");
    require(di.mma.support_class == sbt::FirstBatchMmaClass::CommittedDirectNative, "committed direct-native");
    require(di.mma.lowering_class == sbt::MmaLoweringClass::NativeMmaSync, "native mma.sync");
  }

  {
    const auto di = decode_one_word(make_mma_word(
        /*abtype=*/1u, /*shape=*/3u, /*alayout=row=*/false, /*blayout=col=*/false, /*cdtype=*/0u, /*vd=*/7u, /*vs1=*/8u, /*vs2=*/9u));
    require(di.mma.shape == sbt::MmaShape::M16N16K16, "shape m16n16k16");
    require(di.mma.cd_type == sbt::MmaCdType::Fp16, "cd fp16");
    require(di.mma.a_regs_per_thread == 4, "a regs n16");
    require(di.mma.b_regs_per_thread == 4, "b regs n16");
    require(di.mma.c_regs_per_thread == 8, "c regs n16");
    require(di.mma.support_class == sbt::FirstBatchMmaClass::CommittedSplitNComposite, "committed split-n");
    require(di.mma.lowering_class == sbt::MmaLoweringClass::CompositeLowering, "composite lowering");
  }

  {
    const auto di = decode_one_word(make_mma_word(
        /*abtype=*/2u, /*shape=*/1u, /*alayout=row=*/false, /*blayout=row=*/true, /*cdtype=*/1u, /*vd=*/10u, /*vs1=*/11u, /*vs2=*/12u));
    require(di.mma.shape == sbt::MmaShape::M16N8K16, "shape m16n8k16 deferred");
    require(di.mma.b_layout == sbt::MmaLayout::Row, "b row");
    require(di.mma.spike_b_row_layout, "spike b row bit");
    require(di.mma.support_class == sbt::FirstBatchMmaClass::Deferred, "deferred family");
    require(di.mma.lowering_class == sbt::MmaLoweringClass::Unsupported, "deferred not yet lowerable");
  }

  {
    const auto di = decode_one_word(make_mma_word(
        /*abtype=*/1u, /*shape=*/0u, /*alayout=row=*/false, /*blayout=col=*/false, /*cdtype=*/1u, /*vd=*/13u, /*vs1=*/14u, /*vs2=*/15u));
    require(di.mma.shape == sbt::MmaShape::M8N8K16, "shape m8n8k16");
    require(di.mma.a_regs_per_thread == 2, "a regs m8");
    require(di.mma.b_regs_per_thread == 2, "b regs m8");
    require(di.mma.c_regs_per_thread == 2, "c regs m8");
    require(di.mma.support_class == sbt::FirstBatchMmaClass::Research, "research family");
  }

  {
    const auto di = decode_one_word(make_mma_word(
        /*abtype=*/0u, /*shape=*/5u, /*alayout=row=*/false, /*blayout=col=*/false, /*cdtype=*/1u, /*vd=*/16u, /*vs1=*/17u, /*vs2=*/18u));
    require(di.mma.shape == sbt::MmaShape::M16N8K8, "shape m16n8k8 tf32");
    require(di.mma.ab_type == sbt::MmaAbType::Tf32, "ab tf32");
    require(di.mma.wide_ab, "tf32 is wide ab");
    require(di.mma.support_class == sbt::FirstBatchMmaClass::CommittedDirectNative, "tf32 committed direct");
    require(di.mma.lowering_class == sbt::MmaLoweringClass::NativeMmaSync, "tf32 native mma.sync");
  }

  require(decode_should_throw(
              make_mma_word(/*abtype=*/0u, /*shape=*/1u, /*alayout=*/false, /*blayout=*/false, /*cdtype=*/1u, /*vd=*/1u, /*vs1=*/2u, /*vs2=*/3u)),
          "tf32 with k16 shape stays fail-fast under require-known");
  require(decode_should_throw(
              make_mma_word(/*abtype=*/3u, /*shape=*/5u, /*alayout=*/false, /*blayout=*/false, /*cdtype=*/1u, /*vd=*/1u, /*vs1=*/2u, /*vs2=*/3u)),
          "unknown abtype stays fail-fast under require-known");

  std::cout << "ok mma decode path\n";
  return 0;
}
