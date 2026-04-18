#include "sbt/cfg_verify.hpp"
#include "sbt/control_semantics.hpp"
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
  if (!ok)
    throw std::runtime_error("assert: " + msg);
}

template <class Fn>
void require_throw_contains(Fn &&fn, const std::string &needle,
                            const std::string &msg) {
  bool threw = false;
  try {
    fn();
  } catch (const std::runtime_error &e) {
    threw = std::string(e.what()).find(needle) != std::string::npos;
  }
  require(threw, msg);
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t w) {
  out.push_back(static_cast<uint8_t>(w & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
}

uint32_t make_i_word(uint32_t opcode, uint32_t funct3, uint32_t rd,
                     uint32_t rs1, uint32_t imm12) {
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (rd & 0x1Fu) << 7;
  w |= (funct3 & 0x7u) << 12;
  w |= (rs1 & 0x1Fu) << 15;
  w |= (imm12 & 0xFFFu) << 20;
  return w;
}

uint32_t make_b_word(uint32_t funct3, uint32_t rs1, uint32_t rs2,
                     int32_t imm13) {
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
  if (bi.inst.inst_id == sbt::kUnknownInstId && name != "unknown")
    bi.inst.inst_id = sbt::make_inst_id(name);
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
  bb0.succs = {
      sbt::cfg::Edge{kStart, kStart + 8u, sbt::cfg::EdgeKind::Fallthrough}};

  sbt::cfg::BasicBlock bb1;
  bb1.start = kStart + 8u;
  bb1.inst_indices = {2, 3, 4};
  bb1.succs = {
      sbt::cfg::Edge{kStart + 8u, kJoinPc, sbt::cfg::EdgeKind::Branch},
      sbt::cfg::Edge{kStart + 8u, kStart + 20u,
                     sbt::cfg::EdgeKind::Fallthrough},
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

sbt::cfg::FunctionCfg make_barrier_cfg(uint32_t start) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = start + 8u;
  cfg.insts = {make_inst(start, "barrier"), make_inst(start + 4u, "endprg")};
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    cfg.inst_index_by_pc.emplace(cfg.insts[i].pc, i);
    cfg.inst_pc_to_block.emplace(cfg.insts[i].pc, start);
  }
  cfg.block_index_by_start.emplace(start, 0);
  cfg.blocks.push_back(sbt::cfg::BasicBlock{start, {0, 1}, {}});
  return cfg;
}

void check_spike_want_sync() {
  const auto want =
      sbt::load_spike_want_list(std::filesystem::path("data/spike_want.txt"));
  std::unordered_set<std::string> metadata_names;
  for (const auto &metadata : sbt::all_inst_metadata()) {
    if (!metadata.spike_managed)
      continue;
    metadata_names.emplace(metadata.name);
    require(
        want.set.contains(metadata.name),
        "spike metadata entry must stay reachable from data/spike_want.txt");
  }
  for (const auto &id : want.ids) {
    require(metadata_names.contains(id),
            "want entry must have shared Spike metadata: " + id);
  }
}

void check_decode_metadata_population() {
  const uint32_t word = make_i_word(/*opcode=*/0x13u, /*funct3=*/0x0u,
                                    /*rd=*/3u, /*rs1=*/11u, /*imm12=*/0x15u);
  std::vector<uint8_t> text;
  append_u32_le(text, word);

  sbt::DecodeOptions opt;
  opt.require_known = true;
  const std::vector<sbt::Pattern> patterns = {{
      sbt::Pattern{"vadd12_vi", word, 0xFFFF'FFFFu},
  }};
  const auto decoded =
      sbt::decode_text(text, /*text_vaddr=*/0x80000000u, opt, patterns);
  require(decoded.size() == 1, "single Spike-backed instruction decoded");
  require(decoded[0].inst_id != sbt::kUnknownInstId,
          "shared inst id populated");
  require(decoded[0].operand_form == sbt::OperandForm::VRdRs1VectorImm,
          "operand form comes from shared metadata");
  require(decoded[0].rs1_class == sbt::RegClass::V &&
              decoded[0].rs2_class == sbt::RegClass::None,
          "special vi-12 shape no longer guessed from suffix");
  require(decoded[0].imm_kind == sbt::ImmKind::I12,
          "imm kind comes from shared metadata");
  require(decoded[0].uniform_transfer_kind ==
              sbt::UniformTransferKind::UniformIfRs1,
          "uniform transfer metadata populated");
}

void check_missing_spike_metadata_fails() {
  const uint32_t word =
      make_b_word(/*funct3=*/0x0u, /*rs1=*/1u, /*rs2=*/2u, /*imm13=*/8);
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
    threw = std::string(e.what()).find("missing shared instruction metadata") !=
            std::string::npos;
  }
  require(threw, "Spike-backed decode must fail explicitly when shared "
                 "metadata is missing");
}

void check_missing_scalar_metadata_fails() {
  const uint32_t word = make_i_word(/*opcode=*/0x73u, /*funct3=*/0x4u,
                                    /*rd=*/1u, /*rs1=*/2u, /*imm12=*/3u);
  std::vector<uint8_t> text;
  append_u32_le(text, word);

  sbt::DecodeOptions opt;
  opt.require_known = true;
  bool threw = false;
  try {
    (void)sbt::decode_text(text, /*text_vaddr=*/0x82000000u, opt, {});
  } catch (const std::runtime_error &e) {
    threw = std::string(e.what()).find(
                "missing shared instruction metadata for scalar instruction") !=
            std::string::npos;
  }
  require(threw,
          "scalar decode must fail explicitly when shared metadata is missing");
}
void check_cfg_verify_uses_shared_metadata() {
  auto cfg = make_uniform_vbranch_cfg();
  const auto result = sbt::cfg::verify_function(cfg, "metadata_uniform");
  require(result.vbranch.size() == 1, "expect one vbranch");
  require(result.vbranch[0].proven_uniform,
          "vadd12_vi now propagates uniformity via shared metadata");

  cfg.insts[3].inst.uniform_transfer_kind = sbt::UniformTransferKind::Unknown;
  bool threw = false;
  try {
    (void)sbt::cfg::verify_function(cfg, "metadata_missing");
  } catch (const std::runtime_error &e) {
    threw =
        std::string(e.what()).find(
            "missing shared uniform-transfer metadata") != std::string::npos;
  }
  require(threw, "cfg verify must fail explicitly when supported-path metadata "
                 "is missing");
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

  const auto result =
      sbt::cfg::verify_function(cfg, "repo_local_custom_conservative");
  require(result.vbranch.size() == 1,
          "expect one vbranch for custom uniform check");
  require(
      !result.vbranch[0].proven_uniform,
      "repo-local custom vector op should remain conservatively non-uniform");
}
void check_cfg_build_uses_shared_control_semantics() {
  constexpr uint32_t kStart = 0x1800u;
  constexpr uint32_t kJoinPc = kStart + 0x14u;
  constexpr uint32_t kExitPc = kStart + 0x18u;

  auto branch = make_inst(kStart + 4u, "beq");
  branch.inst.rs1 = 5;
  branch.inst.rs2 = 6;
  branch.inst.imm = static_cast<int32_t>(kJoinPc - branch.inst.pc);
  sbt::finalize_emit_descriptor(branch.inst);
  branch.inst.name = "poison_scalar_branch";
  branch.inst.had_regext = true;
  branch.inst.regext.valid = true;
  branch.inst.regext.pc = kStart;
  branch.inst.regext.prefix_bytes = 4;

  auto call = make_inst(kStart + 8u, "jal");
  call.inst.rd = 1;
  call.inst.imm = 0x200;
  sbt::finalize_emit_descriptor(call.inst);
  call.inst.name = "poison_direct_call";

  auto jump = make_inst(kStart + 12u, "jal");
  jump.inst.rd = 0;
  jump.inst.imm = static_cast<int32_t>(kExitPc - jump.inst.pc);
  sbt::finalize_emit_descriptor(jump.inst);
  jump.inst.name = "poison_direct_jump";

  auto fallthrough_end = make_inst(kStart + 16u, "endprg");
  fallthrough_end.inst.name = "poison_endprg_fallthrough";
  auto join = make_inst(kJoinPc, "join");
  join.inst.name = "poison_join";
  auto exit_end = make_inst(kExitPc, "endprg");
  exit_end.inst.name = "poison_endprg_exit";

  std::vector<sbt::DecodedInst> decoded = {branch.inst, call.inst, jump.inst,
                                           fallthrough_end.inst, join.inst,
                                           exit_end.inst};

  const auto cfg = sbt::cfg::build_function_cfg(decoded, kStart, kExitPc + 4u);
  require(cfg.blocks.size() == 5,
          "cfg must keep call fallthrough, branch target, jump target, and "
          "poisoned join leader");
  require(cfg.blocks[0].start == kStart,
          "regext bundle pc must remain block start");
  require(cfg.blocks[1].start == kStart + 8u,
          "terminator fallthrough leader must use inst_pc + 4");
  require(cfg.blocks[3].start == kJoinPc,
          "join must remain leader even when mnemonic is poisoned");

  require(cfg.blocks[0].succs.size() == 2,
          "branch block must keep branch + fallthrough edges");
  require(cfg.blocks[0].succs[0].dst == kJoinPc ||
              cfg.blocks[0].succs[1].dst == kJoinPc,
          "branch target must use inst_pc, not bundle pc");
  require(cfg.blocks[1].inst_indices.size() == 2,
          "direct call must remain non-terminating so call+jump share a block");
  require(cfg.blocks[1].succs.size() == 1 &&
              cfg.blocks[1].succs[0].dst == kExitPc,
          "poisoned jump must still produce jump edge");
  require(cfg.blocks[3].succs.size() == 1 &&
              cfg.blocks[3].succs[0].dst == kExitPc,
          "join block must still fall through to exit");
}
void check_cfg_verify_control_semantics_are_name_independent() {
  auto cfg = make_uniform_vbranch_cfg();
  cfg.insts[0].inst.name = "poison_auipc";
  cfg.insts[1].inst.name = "poison_setrpc";
  cfg.insts[4].inst.name = "poison_vbranch";
  cfg.insts[6].inst.name = "poison_join";

  const auto result = sbt::cfg::verify_function(cfg, "poisoned_verify");
  require(result.vbranch.size() == 1,
          "poisoned vbranch verify should still find one vbranch");
  require(result.vbranch[0].error.empty(),
          "poisoned structured-control names must not break verify");
  require(
      result.vbranch[0].join_is_join_inst,
      "join resolution must use structured metadata, not join mnemonic text");

  auto barrier_cfg = make_barrier_cfg(0x1900u);
  barrier_cfg.insts[0].inst.name = "poison_barrier";
  const auto barrier_result =
      sbt::cfg::verify_function(barrier_cfg, "poisoned_barrier");
  require(barrier_result.barriers.size() == 1,
          "poisoned barrier verify should still detect the barrier");
  require(barrier_result.barriers[0].ok,
          "barrier outside divergent region must stay valid");

  auto jalr = make_inst(0x1910u, "jalr");
  jalr.inst.rd = 0;
  jalr.inst.rs1 = 5;
  jalr.inst.imm = 12;
  sbt::finalize_emit_descriptor(jalr.inst);
  jalr.inst.name = "poison_nonret_jalr";
  auto jalr_cfg = sbt::cfg::build_function_cfg({jalr.inst}, 0x1910u, 0x1914u);
  const auto jalr_result =
      sbt::cfg::verify_function(jalr_cfg, "poisoned_unsupported_jalr");
  require(jalr_result.unsupported_jalr.size() == 1,
          "poisoned non-ret jalr must still be reported as unsupported");
  require(jalr_result.unsupported_jalr[0].addr == 0x1910u,
          "unsupported jalr report must keep the original instruction pc");
}
void check_direct_call_scan_uses_shared_control_semantics() {
  constexpr uint32_t kStart = 0x1a00u;
  constexpr uint32_t kTarget = 0x1c00u;

  auto regext_call = make_inst(kStart + 4u, "jal");
  regext_call.inst.rd = 1;
  regext_call.inst.imm = static_cast<int32_t>(kTarget - regext_call.inst.pc);
  sbt::finalize_emit_descriptor(regext_call.inst);
  regext_call.inst.name = "poison_regext_call";
  regext_call.inst.had_regext = true;
  regext_call.inst.regext.valid = true;
  regext_call.inst.regext.pc = kStart;
  regext_call.inst.regext.prefix_bytes = 4;

  auto duplicate_call = make_inst(kStart + 8u, "jal");
  duplicate_call.inst.rd = 5;
  duplicate_call.inst.imm =
      static_cast<int32_t>(kTarget - duplicate_call.inst.pc);
  sbt::finalize_emit_descriptor(duplicate_call.inst);
  duplicate_call.inst.name = "poison_duplicate_call";

  auto cfg = sbt::cfg::build_function_cfg(
      {regext_call.inst, duplicate_call.inst}, kStart, kStart + 12u);
  const auto targets =
      sbt::control::collect_direct_call_targets(cfg, "poisoned_calls");
  require(targets.size() == 1,
          "direct-call scan should deduplicate identical call targets");
  require(targets[0] == kTarget,
          "direct-call target must use inst_pc even for regext-bundled call");
}
void check_control_semantics_fail_fast() {
  auto broken_branch = make_inst(0x1b00u, "beq");
  broken_branch.inst.rs1 = 1;
  broken_branch.inst.rs2 = 2;
  broken_branch.inst.imm = 8;
  sbt::finalize_emit_descriptor(broken_branch.inst);
  broken_branch.inst.name = "poison_missing_branch_semantics";
  broken_branch.inst.emit.control_kind = sbt::ControlKind::None;
  const auto broken_end = make_inst(0x1b04u, "endprg");
  require_throw_contains([&]() { (void)sbt::cfg::build_function_cfg({broken_branch.inst, broken_end.inst}, 0x1b00u, 0x1b08u); }, "missing control_kind",
      "cfg build must fail explicitly when supported branch semantics are "
      "missing");
  auto broken_verify_cfg = make_uniform_vbranch_cfg();
  broken_verify_cfg.insts[1].inst.emit.structured_control_kind =
      sbt::StructuredControlKind::None;
  require_throw_contains([&]() { (void)sbt::cfg::verify_function(broken_verify_cfg, "broken_verify"); }, "missing structured_control_kind",
      "cfg verify must fail explicitly when structured control semantics are "
      "missing");
  auto broken_call = make_inst(0x1c00u, "jal");
  broken_call.inst.rd = 1;
  broken_call.inst.imm = 0x40;
  sbt::finalize_emit_descriptor(broken_call.inst);
  auto broken_cfg =
      sbt::cfg::build_function_cfg({broken_call.inst}, 0x1c00u, 0x1c04u);
  broken_cfg.insts[0].inst.emit.control_kind = sbt::ControlKind::None;
  require_throw_contains([&]() { (void)sbt::control::collect_direct_call_targets(broken_cfg, "broken_call"); }, "missing control_kind",
      "direct-call scan must fail explicitly when direct-call semantics are "
      "missing");
}
} // namespace
int main() {
  check_spike_want_sync();
  check_decode_metadata_population();
  check_missing_spike_metadata_fails();
  check_missing_scalar_metadata_fails();
  check_cfg_verify_uses_shared_metadata();
  check_cfg_verify_keeps_repo_local_custom_conservative();
  check_cfg_build_uses_shared_control_semantics();
  check_cfg_verify_control_semantics_are_name_independent();
  check_direct_call_scan_uses_shared_control_semantics();
  check_control_semantics_fail_fast();
  std::cout << "ok instruction metadata contract\n";
  return 0;
}
