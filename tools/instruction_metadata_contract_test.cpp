#include "sbt/cfg_verify.hpp"
#include "sbt/riscv_decode.hpp"
#include "sbt/want_file.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
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

uint32_t make_i_word(uint32_t opcode, uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t imm12) {
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (rd & 0x1Fu) << 7;
  w |= (funct3 & 0x7u) << 12;
  w |= (rs1 & 0x1Fu) << 15;
  w |= (imm12 & 0xFFFu) << 20;
  return w;
}

uint32_t make_b_word(uint32_t funct3, uint32_t rs1, uint32_t rs2, int32_t imm13) {
  const uint32_t u = static_cast<uint32_t>(imm13);
  uint32_t w = 0x63u;
  w |= ((u >> 11) & 0x1u) << 7;
  w |= ((u >> 1) & 0xFu) << 8;
  w |= (funct3 & 0x7u) << 12;
  w |= (rs1 & 0x1Fu) << 15;
  w |= (rs2 & 0x1Fu) << 20;
  w |= ((u >> 5) & 0x3Fu) << 25;
  w |= ((u >> 12) & 0x1u) << 31;
  return w;
}

uint32_t make_j_word(uint32_t rd, int32_t imm21) {
  const uint32_t u = static_cast<uint32_t>(imm21);
  uint32_t w = 0x6Fu;
  w |= (rd & 0x1Fu) << 7;
  w |= ((u >> 12) & 0xFFu) << 12;
  w |= ((u >> 11) & 0x1u) << 20;
  w |= ((u >> 1) & 0x3FFu) << 21;
  w |= ((u >> 20) & 0x1u) << 31;
  return w;
}

sbt::cfg::BundleInst make_inst(uint32_t pc, const std::string &name) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = name;
  (void)sbt::populate_inst_metadata(name, bi.inst);
  sbt::finalize_emit_descriptor(bi.inst);
  if (bi.inst.inst_id == sbt::kUnknownInstId && name != "unknown") bi.inst.inst_id = sbt::make_inst_id(name);
  return bi;
}

sbt::cfg::FunctionCfg make_uniform_vbranch_cfg() {
  constexpr uint32_t kStart = 0x1000u;
  constexpr uint32_t kJoinPc = 0x1018u;

  auto auipc = make_inst(kStart, "auipc");
  auipc.inst.rd = 1;
  auipc.inst.imm = 0;
  sbt::finalize_emit_descriptor(auipc.inst);

  auto setrpc = make_inst(kStart + 4u, "setrpc");
  setrpc.inst.rs1 = 1;
  setrpc.inst.imm = static_cast<int32_t>(kJoinPc - kStart);
  sbt::finalize_emit_descriptor(setrpc.inst);

  auto vmv_vi = make_inst(kStart + 8u, "vmv_v_i");
  vmv_vi.inst.rd = 2;
  vmv_vi.inst.imm = 3;
  sbt::finalize_emit_descriptor(vmv_vi.inst);

  auto vadd12 = make_inst(kStart + 12u, "vadd12_vi");
  vadd12.inst.rd = 1;
  vadd12.inst.rs1 = 2;
  vadd12.inst.imm = 7;
  sbt::finalize_emit_descriptor(vadd12.inst);

  auto vbeq = make_inst(kStart + 16u, "vbeq");
  vbeq.inst.rs1 = 1;
  vbeq.inst.rs2 = 1;
  vbeq.inst.imm = static_cast<int32_t>(kJoinPc - (kStart + 16u));
  sbt::finalize_emit_descriptor(vbeq.inst);

  auto jump = make_inst(kStart + 20u, "jal");
  jump.inst.rd = 0;
  jump.inst.imm = static_cast<int32_t>(kJoinPc - (kStart + 20u));
  sbt::finalize_emit_descriptor(jump.inst);

  auto join = make_inst(kJoinPc, "join");
  auto endprg = make_inst(kJoinPc + 4u, "endprg");

  sbt::cfg::FunctionCfg cfg;
  cfg.start = kStart;
  cfg.end = kJoinPc + 8u;
  cfg.insts = {auipc, setrpc, vmv_vi, vadd12, vbeq, jump, join, endprg};

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    cfg.inst_index_by_pc.emplace(cfg.insts[i].pc, i);
  }
  cfg.inst_pc_to_block.emplace(kStart, kStart);
  cfg.inst_pc_to_block.emplace(kStart + 4u, kStart);
  cfg.inst_pc_to_block.emplace(kStart + 8u, kStart + 8u);
  cfg.inst_pc_to_block.emplace(kStart + 12u, kStart + 8u);
  cfg.inst_pc_to_block.emplace(kStart + 16u, kStart + 8u);
  cfg.inst_pc_to_block.emplace(kStart + 20u, kStart + 20u);
  cfg.inst_pc_to_block.emplace(kJoinPc, kJoinPc);
  cfg.inst_pc_to_block.emplace(kJoinPc + 4u, kJoinPc);

  sbt::cfg::BasicBlock bb0;
  bb0.start = kStart;
  bb0.inst_indices = {0, 1};
  bb0.succs = {sbt::cfg::Edge{kStart, kStart + 8u, sbt::cfg::EdgeKind::Fallthrough}};

  sbt::cfg::BasicBlock bb1;
  bb1.start = kStart + 8u;
  bb1.inst_indices = {2, 3, 4};
  bb1.succs = {
      sbt::cfg::Edge{kStart + 8u, kJoinPc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{kStart + 8u, kStart + 20u, sbt::cfg::EdgeKind::Fallthrough},
  };

  sbt::cfg::BasicBlock bb2;
  bb2.start = kStart + 20u;
  bb2.inst_indices = {5};
  bb2.succs = {sbt::cfg::Edge{kStart + 20u, kJoinPc, sbt::cfg::EdgeKind::Jump}};

  sbt::cfg::BasicBlock bb3;
  bb3.start = kJoinPc;
  bb3.inst_indices = {6, 7};

  cfg.block_index_by_start.emplace(bb0.start, 0);
  cfg.block_index_by_start.emplace(bb1.start, 1);
  cfg.block_index_by_start.emplace(bb2.start, 2);
  cfg.block_index_by_start.emplace(bb3.start, 3);
  cfg.blocks = {bb0, bb1, bb2, bb3};
  return cfg;
}

void check_spike_want_sync() {
  const auto want = sbt::load_spike_want_list(std::filesystem::path("data/spike_want.txt"));
  std::unordered_set<std::string> metadata_names;
  for (const auto &metadata : sbt::all_inst_metadata()) {
    if (!metadata.spike_managed) continue;
    metadata_names.emplace(metadata.name);
    require(want.set.contains(metadata.name), "spike metadata entry must stay reachable from data/spike_want.txt");
  }
  for (const auto &id : want.ids) {
    require(metadata_names.contains(id), "want entry must have shared Spike metadata: " + id);
  }
}

void check_decode_metadata_population() {
  const uint32_t word = make_i_word(/*opcode=*/0x13u, /*funct3=*/0x0u, /*rd=*/3u, /*rs1=*/11u, /*imm12=*/0x15u);
  std::vector<uint8_t> text;
  append_u32_le(text, word);

  sbt::DecodeOptions opt;
  opt.require_known = true;
  const std::vector<sbt::Pattern> patterns = {{
      sbt::Pattern{"vadd12_vi", word, 0xFFFF'FFFFu},
  }};
  const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x80000000u, opt, patterns);
  require(decoded.size() == 1, "single Spike-backed instruction decoded");
  require(decoded[0].inst_id != sbt::kUnknownInstId, "shared inst id populated");
  require(decoded[0].operand_form == sbt::OperandForm::VRdRs1VectorImm, "operand form comes from shared metadata");
  require(decoded[0].rs1_class == sbt::RegClass::V && decoded[0].rs2_class == sbt::RegClass::None, "special vi-12 shape no longer guessed from suffix");
  require(decoded[0].imm_kind == sbt::ImmKind::I12, "imm kind comes from shared metadata");
  require(decoded[0].uniform_transfer_kind == sbt::UniformTransferKind::UniformIfRs1, "uniform transfer metadata populated");
}

void check_missing_spike_metadata_fails() {
  const uint32_t word = make_b_word(/*funct3=*/0x0u, /*rs1=*/1u, /*rs2=*/2u, /*imm13=*/8);
  std::vector<uint8_t> text;
  append_u32_le(text, word);

  sbt::DecodeOptions opt;
  opt.require_known = true;
  const std::vector<sbt::Pattern> patterns = {{
      sbt::Pattern{"synthetic_missing_metadata", word, 0xFFFF'FFFFu},
  }};

  bool threw = false;
  try {
    (void)sbt::decode_text(text, /*text_vaddr=*/0x81000000u, opt, patterns);
  } catch (const std::runtime_error &e) {
    threw = std::string(e.what()).find("missing shared instruction metadata") != std::string::npos;
  }
  require(threw, "Spike-backed decode must fail explicitly when shared metadata is missing");
}

void check_missing_scalar_metadata_fails() {
  const uint32_t word = make_i_word(/*opcode=*/0x73u, /*funct3=*/0x4u, /*rd=*/1u, /*rs1=*/2u, /*imm12=*/3u);
  std::vector<uint8_t> text;
  append_u32_le(text, word);

  sbt::DecodeOptions opt;
  opt.require_known = true;
  bool threw = false;
  try {
    (void)sbt::decode_text(text, /*text_vaddr=*/0x82000000u, opt, {});
  } catch (const std::runtime_error &e) {
    threw = std::string(e.what()).find("missing shared instruction metadata for scalar instruction") != std::string::npos;
  }
  require(threw, "scalar decode must fail explicitly when shared metadata is missing");
}

void check_cfg_verify_uses_shared_metadata() {
  auto cfg = make_uniform_vbranch_cfg();
  const auto result = sbt::cfg::verify_function(cfg, "metadata_uniform");
  require(result.vbranch.size() == 1, "expect one vbranch");
  require(result.vbranch[0].proven_uniform, "vadd12_vi now propagates uniformity via shared metadata");

  cfg.insts[3].inst.uniform_transfer_kind = sbt::UniformTransferKind::Unknown;
  bool threw = false;
  try {
    (void)sbt::cfg::verify_function(cfg, "metadata_missing");
  } catch (const std::runtime_error &e) {
    threw = std::string(e.what()).find("missing shared uniform-transfer metadata") != std::string::npos;
  }
  require(threw, "cfg verify must fail explicitly when supported-path metadata is missing");
}

void check_cfg_verify_keeps_repo_local_custom_conservative() {
  auto cfg = make_uniform_vbranch_cfg();
  cfg.insts[2].inst.name = "shuffle_idx";
  cfg.insts[2].inst.inst_id = sbt::make_inst_id(cfg.insts[2].inst.name);
  cfg.insts[2].inst.operand_form = sbt::OperandForm::None;
  cfg.insts[2].inst.uniform_transfer_kind = sbt::UniformTransferKind::Unknown;
  cfg.insts[2].inst.custom.valid = true;
  cfg.insts[2].inst.custom.family = sbt::CustomFamily::Shuffle;
  cfg.insts[2].inst.custom.subop = sbt::CustomSubOp::ShuffleIdx;
  cfg.insts[2].inst.rd_class = sbt::RegClass::V;
  cfg.insts[2].inst.rd = 2;
  cfg.insts[2].inst.rs2_class = sbt::RegClass::V;
  cfg.insts[2].inst.rs2 = 3;
  cfg.insts[2].inst.imm_kind = sbt::ImmKind::UImm5;
  cfg.insts[2].inst.imm = 1;
  sbt::finalize_emit_descriptor(cfg.insts[2].inst);

  const auto result = sbt::cfg::verify_function(cfg, "repo_local_custom_conservative");
  require(result.vbranch.size() == 1, "expect one vbranch for custom uniform check");
  require(!result.vbranch[0].proven_uniform, "repo-local custom vector op should remain conservatively non-uniform");
}

} // namespace

int main() {
  check_spike_want_sync();
  check_decode_metadata_population();
  check_missing_spike_metadata_fails();
  check_missing_scalar_metadata_fails();
  check_cfg_verify_uses_shared_metadata();
  check_cfg_verify_keeps_repo_local_custom_conservative();
  std::cout << "ok instruction metadata contract\n";
  return 0;
}
