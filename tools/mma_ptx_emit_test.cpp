#include "sbt/ptx_emit.hpp"
#include "sbt/ptx_mma.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
}

sbt::cfg::BundleInst make_mma_inst(uint32_t pc, int rd_base, int rs1_base, int rs2_base, sbt::MmaShape shape, sbt::MmaAbType ab_type,
                                   sbt::MmaCdType cd_type, sbt::MmaLayout a_layout, sbt::MmaLayout b_layout,
                                   sbt::FirstBatchMmaClass support_class, sbt::MmaLoweringClass lowering_class,
                                   uint8_t a_regs_per_thread, uint8_t b_regs_per_thread, uint8_t c_regs_per_thread, bool wide_ab) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = std::string("mma_") + sbt::to_string(shape) + "_" + sbt::to_string(a_layout) + "_" + sbt::to_string(b_layout);
  bi.inst.rd_class = sbt::RegClass::V;
  bi.inst.rs1_class = sbt::RegClass::V;
  bi.inst.rs2_class = sbt::RegClass::V;
  bi.inst.rd = rd_base;
  bi.inst.rs1 = rs1_base;
  bi.inst.rs2 = rs2_base;
  bi.inst.custom.valid = true;
  bi.inst.custom.family = sbt::CustomFamily::Mma;
  bi.inst.mma.valid = true;
  bi.inst.mma.shape = shape;
  bi.inst.mma.a_layout = a_layout;
  bi.inst.mma.b_layout = b_layout;
  bi.inst.mma.ab_type = ab_type;
  bi.inst.mma.cd_type = cd_type;
  bi.inst.mma.rd_base = rd_base;
  bi.inst.mma.rs1_base = rs1_base;
  bi.inst.mma.rs2_base = rs2_base;
  bi.inst.mma.a_regs_per_thread = a_regs_per_thread;
  bi.inst.mma.b_regs_per_thread = b_regs_per_thread;
  bi.inst.mma.c_regs_per_thread = c_regs_per_thread;
  bi.inst.mma.wide_ab = wide_ab;
  bi.inst.mma.support_class = support_class;
  bi.inst.mma.lowering_class = lowering_class;
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::BundleInst make_endprg(uint32_t pc) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = "endprg";
  (void)sbt::populate_inst_metadata(bi.inst.name, bi.inst);
  sbt::finalize_emit_descriptor(bi.inst);
  bi.inst.inst_id = sbt::make_inst_id(bi.inst.name);
  return bi;
}

sbt::cfg::FunctionCfg make_cfg(uint32_t start, std::vector<sbt::cfg::BundleInst> insts) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = insts.empty() ? (start + 4u) : (insts.back().inst_pc + 4u);
  cfg.insts = std::move(insts);

  sbt::cfg::BasicBlock bb;
  bb.start = start;
  bb.inst_indices.reserve(cfg.insts.size());
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto pc = cfg.insts[i].pc;
    bb.inst_indices.push_back(i);
    cfg.inst_index_by_pc.emplace(pc, i);
    cfg.inst_pc_to_block.emplace(pc, start);
  }
  cfg.blocks.push_back(std::move(bb));
  cfg.block_index_by_start.emplace(start, 0);
  return cfg;
}

size_t count_substr(const std::string &haystack, const std::string &needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

std::string find_line_containing(const std::string &text, const std::string &needle) {
  const size_t pos = text.find(needle);
  require(pos != std::string::npos, "missing expected PTX line");
  const size_t line_start = text.rfind('\n', pos);
  const size_t line_end = text.find('\n', pos);
  const size_t begin = (line_start == std::string::npos) ? 0 : (line_start + 1u);
  const size_t end = (line_end == std::string::npos) ? text.size() : line_end;
  return text.substr(begin, end - begin);
}

void expect_native_mma_tuple_line(const std::string &ptx, const std::string &opcode, size_t expected_b32_temps, size_t expected_f32_temps) {
  const auto line = find_line_containing(ptx, opcode);
  require(count_substr(line, "%tmp_b32_") == expected_b32_temps, "unexpected b32 temp tuple width in native mma line");
  require(count_substr(line, "%tmp_f32_") == expected_f32_temps, "unexpected f32 temp tuple width in native mma line");
  require(line.find("%r3") == std::string::npos && line.find("%r4") == std::string::npos && line.find("%r7") == std::string::npos,
          "native mma tuple line must not depend on legacy fixed scratch registers");
  require(line.find("%f0") == std::string::npos && line.find("%f1") == std::string::npos,
          "native mma tuple line must not depend on legacy fixed fp scratch registers");
}

void expect_scratchless_mma_shuffle(const std::string &ptx) {
  require(ptx.find("activemask.b32 %r1;") != std::string::npos, "MMA lowering must check the active warp mask");
  require(ptx.find(", %r1, 0xffffffff;") != std::string::npos, "MMA lowering must test for non-full-warp execution");
  require(ptx.find(" trap;") != std::string::npos, "MMA lowering must trap on invalid warp/mapping conditions");
  require(ptx.find("shfl.sync.idx.b32 %tmp_b32_") != std::string::npos, "MMA lowering must use shuffle-based tuple materialization");
  require(ptx.find("ld.shared.u32 %tmp_b32_") == std::string::npos, "MMA lowering must not use temp-address shared loads");
  require(ptx.find("st.shared.u32 [%tmp_b64_") == std::string::npos, "MMA lowering must not use temp-address shared stores");
}

void compile_with_ptxas(const std::string &stem, const std::string &ptx) {
  const auto ptx_path = std::filesystem::temp_directory_path() / (stem + ".ptx");
  const auto cubin_path = std::filesystem::temp_directory_path() / (stem + ".cubin");
  {
    std::ofstream f(ptx_path);
    require(static_cast<bool>(f), "open temp PTX output");
    f << ptx;
  }
  const char *ptxas = std::getenv("PTXAS");
  const std::string ptxas_bin = (ptxas && ptxas[0] != '\0') ? ptxas : "ptxas";
  const std::string cmd = ptxas_bin + " -arch=sm_89 \"" + ptx_path.string() + "\" -o \"" + cubin_path.string() + "\" >/dev/null 2>&1";
  require(std::system(cmd.c_str()) == 0, "ptxas compile-first for mma lowering");
  std::filesystem::remove(ptx_path);
  std::filesystem::remove(cubin_path);
}

void expect_emit_error(const sbt::cfg::FunctionCfg &cfg, const std::string &code, const std::string &message_substr) {
  std::unordered_map<uint32_t, std::string> sym_by_addr;
  sym_by_addr.emplace(cfg.start, "mma_negative");
  sbt::ptx::Options opt;
  opt.sm = 89;
  opt.include_comments = false;
  try {
    (void)sbt::ptx::emit_kernel(cfg, sym_by_addr, "mma_negative", opt);
  } catch (const sbt::ptx::EmitError &e) {
    require(e.code == code, "unexpected EmitError code");
    require(std::string(e.what()).find(message_substr) != std::string::npos, "missing expected EmitError diagnostic");
    return;
  }
  throw std::runtime_error("assert: expected EmitError");
}

std::string emit_success_ptx(const sbt::cfg::FunctionCfg &cfg) {
  std::unordered_map<uint32_t, std::string> sym_by_addr;
  sym_by_addr.emplace(cfg.start, "mma_positive");
  sbt::ptx::Options opt;
  opt.sm = 89;
  opt.include_comments = false;
  return sbt::ptx::emit_kernel(cfg, sym_by_addr, "mma_positive", opt).ptx;
}

void expect_emit_success(const sbt::cfg::FunctionCfg &cfg, const std::string &needle) {
  const auto ptx = emit_success_ptx(cfg);
  require(ptx.find(needle) != std::string::npos, "missing expected mma.sync opcode in PTX");
}

void expect_bf16_planner_tracks_bf16_key() {
  sbt::MmaInstInfo f16_mma;
  f16_mma.valid = true;
  f16_mma.shape = sbt::MmaShape::M16N8K16;
  f16_mma.ab_type = sbt::MmaAbType::Fp16;
  f16_mma.cd_type = sbt::MmaCdType::Fp32;

  sbt::MmaInstInfo bf16_mma = f16_mma;
  bf16_mma.ab_type = sbt::MmaAbType::Bf16;

  const auto *f16_abi = sbt::ptx::mma::find_abi_desc(f16_mma);
  const auto *bf16_abi = sbt::ptx::mma::find_abi_desc(bf16_mma);
  require(f16_abi != nullptr, "missing f16 mma abi");
  require(bf16_abi != nullptr, "missing bf16 mma abi");
  require(f16_abi->key != bf16_abi->key, "bf16 must still use a distinct AbiKey");

  const auto f16_b0 = sbt::ptx::mma::scalar_value_plan(*f16_abi, sbt::ptx::mma::OperandRole::B, 0u);
  const auto bf16_b0 = sbt::ptx::mma::scalar_value_plan(*bf16_abi, sbt::ptx::mma::OperandRole::B, 0u);
  const auto f16_plan = sbt::ptx::mma::b_source_window_plan(f16_mma, *f16_abi, 0u);
  const auto bf16_plan = sbt::ptx::mma::b_source_window_plan(bf16_mma, *bf16_abi, 0u);

  require(f16_b0.tile_col_base == 0u && f16_b0.lane_col_bias == 0u, "unexpected baseline f16 B tuple slot");
  require(bf16_b0.tile_col_base == 0u && bf16_b0.lane_col_bias == 0u,
          "bf16 B tuple slot should currently follow the same source order as f16");
  require(f16_plan.half_xor == 0u, "f16 B source planner must keep natural half order");
  require(bf16_plan.half_xor == 0u, "bf16 B source planner must keep the native packed-half order");

  const auto bf16_c0 = sbt::ptx::mma::scalar_value_plan(*bf16_abi, sbt::ptx::mma::OperandRole::C, 0u);
  const auto bf16_c1 = sbt::ptx::mma::scalar_value_plan(*bf16_abi, sbt::ptx::mma::OperandRole::C, 1u);
  const auto bf16_c1_slot = sbt::ptx::mma::scalar_tuple_plan(*bf16_abi, sbt::ptx::mma::OperandRole::C, 1u, 0u);
  require(bf16_c0.tile_row_base == 0u && bf16_c0.lane_row_shift == 2u && bf16_c0.lane_col_mask == 3u && bf16_c0.lane_col_shift == 1u &&
              bf16_c0.lane_col_bias == 0u,
          "bf16 C tuple slot 0 must follow the native m16n8 f32 accumulator ABI");
  require(bf16_c1.tile_row_base == 0u && bf16_c1.lane_row_shift == 2u && bf16_c1.lane_col_mask == 3u && bf16_c1.lane_col_shift == 1u &&
              bf16_c1.lane_col_bias == 1u,
          "bf16 C tuple slot 1 must keep the paired-lane native accumulator ABI");
  require(bf16_c1_slot.lane_col_bias == bf16_c1.lane_col_bias,
          "bf16 C tuple slot planner must resolve by tuple slot, not only linear value id");

  const auto bf16_d1 = sbt::ptx::mma::scalar_value_plan(*bf16_abi, sbt::ptx::mma::OperandRole::D, 1u);
  const auto bf16_d1_slot = sbt::ptx::mma::scalar_tuple_plan(*bf16_abi, sbt::ptx::mma::OperandRole::D, 1u, 0u);
  require(bf16_d1.lane_xor_mask == 0u && bf16_d1.lane_col_bias == 1u,
          "bf16 D tuple slot 1 must keep the native odd-column writeback slot");
  require(bf16_d1_slot.lane_col_bias == bf16_d1.lane_col_bias,
          "bf16 D writeback planner must resolve by D tuple slot");
}

void expect_split_n_b_source_window_is_slice_aware() {
  sbt::MmaInstInfo mma;
  mma.valid = true;
  mma.shape = sbt::MmaShape::M16N16K8;
  mma.b_layout = sbt::MmaLayout::Col;
  mma.ab_type = sbt::MmaAbType::Tf32;
  mma.cd_type = sbt::MmaCdType::Fp32;
  mma.b_regs_per_thread = 4;

  const auto *abi = sbt::ptx::mma::find_abi_desc(mma);
  require(abi != nullptr, "missing tf32 mma abi");

  const auto slice0 = sbt::ptx::mma::b_source_window_plan(mma, *abi, 0u);
  const auto slice8 = sbt::ptx::mma::b_source_window_plan(mma, *abi, 8u);

  require(slice0.reg_offset == 0u && slice0.reg_count == 4u, "split-n first B slice keeps the full source carrier window");
  require(slice0.logical_n_offset == 0u && slice0.source_window_n == 16u && slice0.native_window_n == 8u,
          "split-n first B slice must distinguish source n-span and native n-span");
  require(slice0.source_window_k == 8u && slice0.source_pack == sbt::ptx::mma::PackMode::Wide32, "tf32 split-n B planner must carry k-span and pack mode");
  require(slice8.reg_offset == 0u && slice8.reg_count == 4u, "split-n second B slice also keeps the full source carrier window");
  require(slice8.logical_n_offset == 8u && slice8.source_window_n == 16u && slice8.native_window_n == 8u,
          "split-n second B slice must carry the logical n offset separately from the native n-span");
  require(slice8.source_window_k == 8u && slice8.source_pack == sbt::ptx::mma::PackMode::Wide32, "split-n second B slice must retain the same source carrier semantics");
}

} // namespace

int main() {
  expect_bf16_planner_tracks_bf16_key();
  expect_split_n_b_source_window_is_slice_aware();

  {
    const auto ptx = emit_success_ptx(
        make_cfg(0x80004000u, {make_mma_inst(0x80004000u, 32, 64, 80, sbt::MmaShape::M16N8K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp16,
                                             sbt::MmaLayout::Row, sbt::MmaLayout::Col, sbt::FirstBatchMmaClass::CommittedDirectNative,
                                             sbt::MmaLoweringClass::NativeMmaSync, 4, 2, 4, false),
                                make_endprg(0x80004004u)}));
    require(ptx.find("mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16") != std::string::npos, "missing fp16 native opcode");
    require(ptx.find(".reg .b32 %tmp_b32_0;") != std::string::npos, "missing fp16 mma b32 temp declaration");
    expect_scratchless_mma_shuffle(ptx);
    expect_native_mma_tuple_line(ptx, "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16", 10u, 0u);
    compile_with_ptxas("mma_ptx_emit_test_fp16_native", ptx);
  }

  {
    const auto ptx = emit_success_ptx(
        make_cfg(0x80004100u, {make_mma_inst(0x80004100u, 32, 64, 80, sbt::MmaShape::M16N8K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp32,
                                             sbt::MmaLayout::Row, sbt::MmaLayout::Col, sbt::FirstBatchMmaClass::CommittedDirectNative,
                                             sbt::MmaLoweringClass::NativeMmaSync, 4, 2, 4, false),
                                make_endprg(0x80004104u)}));
    require(ptx.find("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32") != std::string::npos, "missing expected mma.sync opcode in PTX");
    require(ptx.find(".reg .f32 %tmp_f32_0;") != std::string::npos, "missing fp32 mma f32 temp declaration");
    require(ptx.find(".reg .b32 %tmp_b32_0;") != std::string::npos, "missing fp32 mma b32 temp declaration");
    expect_scratchless_mma_shuffle(ptx);
    expect_native_mma_tuple_line(ptx, "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32", 6u, 8u);
    compile_with_ptxas("mma_ptx_emit_test_fp32_native", ptx);
  }

  {
    const auto ptx = emit_success_ptx(
        make_cfg(0x80004180u, {make_mma_inst(0x80004180u, 32, 64, 80, sbt::MmaShape::M16N16K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp32,
                                             sbt::MmaLayout::Row, sbt::MmaLayout::Col, sbt::FirstBatchMmaClass::CommittedSplitNComposite,
                                             sbt::MmaLoweringClass::CompositeLowering, 4, 4, 8, false),
                                make_endprg(0x80004184u)}));
    require(count_substr(ptx, "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32") == 2u,
            "split-n fp32 lowering must emit exactly two native m16n8k16 ops");
    require(ptx.find("%tmp_b32_") != std::string::npos && ptx.find("%tmp_f32_") != std::string::npos,
            "split-n fp32 lowering should rely on virtual temps");
    expect_scratchless_mma_shuffle(ptx);
    compile_with_ptxas("mma_ptx_emit_test_fp32_split_n", ptx);
  }

  {
    const auto ptx = emit_success_ptx(
        make_cfg(0x800041c0u, {make_mma_inst(0x800041c0u, 32, 64, 80, sbt::MmaShape::M16N16K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp16,
                                             sbt::MmaLayout::Row, sbt::MmaLayout::Col, sbt::FirstBatchMmaClass::CommittedSplitNComposite,
                                             sbt::MmaLoweringClass::CompositeLowering, 4, 4, 8, false),
                                make_endprg(0x800041c4u)}));
    require(count_substr(ptx, "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16") == 2u,
            "split-n fp16 lowering must emit exactly two native m16n8k16 ops");
    require(ptx.find("%tmp_b32_") != std::string::npos, "split-n fp16 lowering should rely on virtual temps");
    expect_scratchless_mma_shuffle(ptx);
    compile_with_ptxas("mma_ptx_emit_test_fp16_split_n", ptx);
  }

  expect_emit_error(
      make_cfg(0x800041e0u, {make_mma_inst(0x800041e0u, 32, 64, 80, sbt::MmaShape::M16N8K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp16,
                                           sbt::MmaLayout::Row, sbt::MmaLayout::Row, sbt::FirstBatchMmaClass::Deferred,
                                           sbt::MmaLoweringClass::Unsupported, 4, 2, 4, false),
                              make_endprg(0x800041e4u)}),
      "unsupported.mma.deferred", "support=deferred");

  try {
    std::unordered_map<uint32_t, std::string> sym_by_addr;
    sym_by_addr.emplace(0x80004200u, "mma_deferred");
    sbt::ptx::Options opt;
    opt.sm = 89;
    opt.include_comments = false;
    (void)sbt::ptx::emit_kernel(
        make_cfg(0x80004200u, {make_mma_inst(0x80004200u, 32, 64, 80, sbt::MmaShape::M16N8K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp32,
                                             sbt::MmaLayout::Row, sbt::MmaLayout::Row, sbt::FirstBatchMmaClass::Deferred,
                                             sbt::MmaLoweringClass::Unsupported, 4, 2, 4, false),
                                make_endprg(0x80004204u)}),
        sym_by_addr, "mma_deferred", opt);
    throw std::runtime_error("assert: expected deferred EmitError");
  } catch (const sbt::ptx::EmitError &e) {
    require(e.code == "unsupported.mma.deferred", "expected deferred error code");
  }

  try {
    std::unordered_map<uint32_t, std::string> sym_by_addr;
    sym_by_addr.emplace(0x80004300u, "mma_research");
    sbt::ptx::Options opt;
    opt.sm = 89;
    opt.include_comments = false;
    (void)sbt::ptx::emit_kernel(
        make_cfg(0x80004300u, {make_mma_inst(0x80004300u, 32, 64, 80, sbt::MmaShape::M8N8K16, sbt::MmaAbType::Fp16, sbt::MmaCdType::Fp32,
                                             sbt::MmaLayout::Row, sbt::MmaLayout::Col, sbt::FirstBatchMmaClass::Research,
                                             sbt::MmaLoweringClass::Unsupported, 2, 2, 2, false),
                                make_endprg(0x80004304u)}),
        sym_by_addr, "mma_research", opt);
    throw std::runtime_error("assert: expected research EmitError");
  } catch (const sbt::ptx::EmitError &e) {
    require(e.code == "unsupported.mma.research", "expected research error code");
  }

  std::cout << "ok mma ptx emit current fp16 support + fail-fast boundaries\n";
  return 0;
}
