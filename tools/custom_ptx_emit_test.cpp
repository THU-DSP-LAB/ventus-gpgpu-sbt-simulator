#include "sbt/ptx_emit.hpp"

#include <cstdlib>
#include <cstdint>
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

sbt::cfg::BundleInst make_custom_inst(uint32_t pc, const std::string &name, int rd, int rs2, int rs1, int imm,
                                      sbt::CustomFamily family, sbt::CustomSubOp subop, sbt::CustomDataType dtype) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = name;
  bi.inst.rd_class = sbt::RegClass::V;
  bi.inst.rd = rd;
  bi.inst.rs2_class = sbt::RegClass::V;
  bi.inst.rs2 = rs2;
  if (rs1 >= 0) {
    bi.inst.rs1_class = sbt::RegClass::V;
    bi.inst.rs1 = rs1;
  }
  if (imm >= 0) {
    bi.inst.imm_kind = sbt::ImmKind::UImm5;
    bi.inst.imm = imm;
  }
  bi.inst.custom.valid = true;
  bi.inst.custom.family = family;
  bi.inst.custom.subop = subop;
  bi.inst.custom.dtype = dtype;
  return bi;
}

sbt::cfg::BundleInst make_endprg(uint32_t pc) {
  sbt::cfg::BundleInst bi;
  bi.pc = pc;
  bi.inst_pc = pc;
  bi.len = 4;
  bi.inst.pc = pc;
  bi.inst.name = "endprg";
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

size_t count_substr(const std::string &text, const std::string &needle) {
  if (needle.empty()) return 0;
  size_t pos = 0;
  size_t n = 0;
  while (true) {
    pos = text.find(needle, pos);
    if (pos == std::string::npos) break;
    ++n;
    pos += needle.size();
  }
  return n;
}

} // namespace

int main() {
  constexpr uint32_t kEntryPc = 0x80002000u;
  std::vector<sbt::cfg::BundleInst> insts;
  insts.push_back(make_custom_inst(kEntryPc + 0u, "shuffle_idx", 1, 2, -1, 3, sbt::CustomFamily::Shuffle, sbt::CustomSubOp::ShuffleIdx,
                                   sbt::CustomDataType::None));
  insts.push_back(make_custom_inst(kEntryPc + 4u, "vcvt_f32_fp16", 3, 4, -1, -1, sbt::CustomFamily::Convert,
                                   sbt::CustomSubOp::CvtF32FromF16, sbt::CustomDataType::Fp16));
  insts.push_back(make_custom_inst(kEntryPc + 8u, "vcvt_bf16_fp32", 5, 6, -1, -1, sbt::CustomFamily::Convert,
                                   sbt::CustomSubOp::CvtBf16FromF32, sbt::CustomDataType::Bf16));
  insts.push_back(make_custom_inst(kEntryPc + 12u, "vadd_f16x2", 7, 8, 9, -1, sbt::CustomFamily::PackedArith, sbt::CustomSubOp::Add,
                                   sbt::CustomDataType::F16x2));
  insts.push_back(make_custom_inst(kEntryPc + 16u, "vfma_bf16x2", 10, 11, 12, -1, sbt::CustomFamily::PackedArith, sbt::CustomSubOp::Fma,
                                   sbt::CustomDataType::Bf16x2));
  insts.push_back(make_custom_inst(kEntryPc + 20u, "vadd_bf16x2", 13, 14, 15, -1, sbt::CustomFamily::PackedArith, sbt::CustomSubOp::Add,
                                   sbt::CustomDataType::Bf16x2));
  insts.push_back(make_custom_inst(kEntryPc + 24u, "vtanh_approx_f32", 16, 17, -1, -1, sbt::CustomFamily::Sfu, sbt::CustomSubOp::Tanh,
                                   sbt::CustomDataType::Fp32));
  insts.push_back(make_custom_inst(kEntryPc + 28u, "vgelu_approx_f16x2", 18, 19, -1, -1, sbt::CustomFamily::Sfu, sbt::CustomSubOp::Gelu,
                                   sbt::CustomDataType::F16x2));
  insts.push_back(make_custom_inst(kEntryPc + 32u, "vsilu_approx_bf16x2", 20, 21, -1, -1, sbt::CustomFamily::Sfu, sbt::CustomSubOp::Silu,
                                   sbt::CustomDataType::Bf16x2));
  insts.push_back(make_endprg(kEntryPc + 36u));

  const auto cfg = make_cfg(kEntryPc, std::move(insts));
  std::unordered_map<uint32_t, std::string> sym_by_addr;
  sym_by_addr.emplace(kEntryPc, "custom_kernel");

  sbt::ptx::Options opt;
  opt.sm = 89;
  opt.include_comments = false;
  const auto res = sbt::ptx::emit_kernel(cfg, sym_by_addr, "custom_kernel", opt);
  const std::string &ptx = res.ptx;

  require(ptx.find(".version 7.8") != std::string::npos, "ptx version baseline");
  require(ptx.find(".target sm_89") != std::string::npos, "sm baseline");
  require(ptx.find("shfl.sync.idx.b32") != std::string::npos, "shuffle lowering");
  require(ptx.find("add.rn.f16x2") != std::string::npos, "f16x2 add lowering");
  require(ptx.find("fma.rn.bf16x2") != std::string::npos, "bf16x2 fma lowering");
  require(ptx.find("cvt.f32.bf16") != std::string::npos, "bf16x2 composite convert up");
  require(ptx.find("cvt.rn.bf16.f32") != std::string::npos, "bf16x2 composite convert down");
  require(count_substr(ptx, "ex2.approx.f32") >= 2, "composed SFU path uses ex2");
  require(ptx.find("unsupported.inst") == std::string::npos, "no unsupported marker in emitted PTX text");

  const auto ptx_path = std::filesystem::temp_directory_path() / "custom_ptx_emit_test.ptx";
  const auto cubin_path = std::filesystem::temp_directory_path() / "custom_ptx_emit_test.cubin";
  {
    std::ofstream f(ptx_path);
    require(static_cast<bool>(f), "open temp PTX output");
    f << ptx;
  }
  const char *ptxas = std::getenv("PTXAS");
  const std::string ptxas_bin = (ptxas && ptxas[0] != '\0') ? ptxas : "ptxas";
  const std::string cmd = ptxas_bin + " -arch=sm_89 \"" + ptx_path.string() + "\" -o \"" + cubin_path.string() + "\" >/dev/null 2>&1";
  require(std::system(cmd.c_str()) == 0, "ptxas compile-first for custom lowering");
  std::filesystem::remove(ptx_path);
  std::filesystem::remove(cubin_path);

  std::cout << "ok custom ptx emit\n";
  return 0;
}
