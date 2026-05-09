#include "sbt/ptx_emit.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
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
  if (bi.inst.inst_id == sbt::kUnknownInstId) {
    bi.inst.inst_id = sbt::make_inst_id(name);
  }
  return bi;
}

sbt::cfg::BundleInst make_pds_vsb(uint32_t pc) {
  auto bi = make_inst(pc, "vsb_v");
  bi.inst.rs1 = 32;
  bi.inst.rs2 = 1;
  bi.inst.imm = -32;
  sbt::finalize_emit_descriptor(bi.inst);
  return bi;
}

sbt::cfg::FunctionCfg make_cfg(uint32_t start, std::vector<sbt::cfg::BundleInst> insts) {
  sbt::cfg::FunctionCfg cfg;
  cfg.start = start;
  cfg.end = insts.back().pc + 4u;
  cfg.insts = std::move(insts);

  sbt::cfg::BasicBlock bb;
  bb.start = start;
  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const uint32_t pc = cfg.insts[i].pc;
    bb.inst_indices.push_back(i);
    cfg.inst_index_by_pc.emplace(pc, i);
    cfg.inst_pc_to_block.emplace(pc, start);
  }
  cfg.block_index_by_start.emplace(start, 0);
  cfg.blocks.push_back(std::move(bb));
  return cfg;
}

} // namespace

int main() {
  constexpr uint32_t kStart = 0x80001000u;
  auto endprg = make_inst(kStart + 4u, "endprg");
  const auto cfg = make_cfg(kStart, {make_pds_vsb(kStart), endprg});

  sbt::ptx::Options opt;
  opt.include_comments = false;
  const auto ptx = sbt::ptx::emit_kernel(cfg, {{kStart, "pds_vsb"}}, "pds_vsb", opt).ptx;

  require(ptx.find("and.b32 %r17, %r14, 3;") != std::string::npos,
          "PDS byte store keeps byte offset within the lane word");
  require(ptx.find("add.u32 %r14, %r14, %r17;") != std::string::npos,
          "PDS byte store applies byte offset to mapped PDS address");
  require(ptx.find("cvt.u8.u32") != std::string::npos,
          "PDS byte store narrows source to byte");
  require(ptx.find("st.shared.u8") != std::string::npos,
          "PDS byte store emits shared byte store path");
  require(ptx.find("st.global.u8") != std::string::npos,
          "PDS byte store emits global byte store path");

  std::cout << "ok PDS vector memory\n";
  return 0;
}
