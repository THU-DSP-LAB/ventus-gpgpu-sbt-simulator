#include "sbt/cfg.hpp"

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace sbt::cfg {
namespace {

static bool is_vbranch(std::string_view name) {
  return name == "vbeq" || name == "vbne" || name == "vblt" || name == "vbge" || name == "vbltu" || name == "vbgeu";
}

static bool is_cond_branch(std::string_view name) {
  return name == "beq" || name == "bne" || name == "blt" || name == "bge" || name == "bltu" || name == "bgeu";
}

static bool is_uncond_jump(const sbt::DecodedInst &di) { return di.name == "jal" && di.rd_class == sbt::RegClass::X && di.rd == 0; }

static bool is_call(const sbt::DecodedInst &di) { return di.name == "jal" && di.rd_class == sbt::RegClass::X && di.rd != 0; }

static bool is_ret(const sbt::DecodedInst &di) {
  return di.name == "jalr" && di.rd_class == sbt::RegClass::X && di.rs1_class == sbt::RegClass::X && di.rd == 0 &&
         di.rs1 == 1 && di.imm_kind == sbt::ImmKind::I12 && di.imm == 0;
}

static bool is_indirect_jalr(const sbt::DecodedInst &di) { return di.name == "jalr" && !is_ret(di); }

struct CfInfo final {
  bool is_terminator = false;
  bool is_branch = false;
  bool is_jump = false;
  bool is_call = false;
  bool is_return = false;
  bool has_fallthrough = true;
  uint32_t target = 0;
};

static CfInfo classify(const BundleInst &bi) {
  const auto &di = bi.inst;
  CfInfo out;

  if (di.name == "endprg") {
    out.is_terminator = true;
    out.has_fallthrough = false;
    return out;
  }
  if (di.name == "join") {
    out.is_terminator = false;
    out.has_fallthrough = true;
    return out;
  }
  if (is_ret(di)) {
    out.is_return = true;
    out.is_terminator = true;
    out.has_fallthrough = false;
    return out;
  }
  if (is_indirect_jalr(di)) {
    out.is_terminator = true;
    out.has_fallthrough = false;
    return out;
  }

  if ((is_cond_branch(di.name) || is_vbranch(di.name)) && di.imm_kind == sbt::ImmKind::B13) {
    const int64_t t = int64_t(bi.inst_pc) + int64_t(di.imm);
    out.is_branch = true;
    out.is_terminator = true;
    out.target = static_cast<uint32_t>(t);
    out.has_fallthrough = true;
    return out;
  }

  if (is_uncond_jump(di) && di.imm_kind == sbt::ImmKind::J21) {
    const int64_t t = int64_t(bi.inst_pc) + int64_t(di.imm);
    out.is_jump = true;
    out.is_terminator = true;
    out.target = static_cast<uint32_t>(t);
    out.has_fallthrough = false;
    return out;
  }

  if (is_call(di) && di.imm_kind == sbt::ImmKind::J21) {
    out.is_call = true;
    out.is_terminator = false;
    out.has_fallthrough = true;
    const int64_t t = int64_t(bi.inst_pc) + int64_t(di.imm);
    out.target = static_cast<uint32_t>(t);
    return out;
  }

  out.is_terminator = false;
  out.has_fallthrough = true;
  return out;
}

static void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error(msg);
}

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

static bool in_range(uint32_t x, uint32_t lo, uint32_t hi_excl) { return x >= lo && x < hi_excl; }

} // namespace

const char *to_string(EdgeKind k) {
  switch (k) {
  case EdgeKind::Branch: return "branch";
  case EdgeKind::Jump: return "jump";
  case EdgeKind::Fallthrough: default: return "fallthrough";
  }
}

FunctionCfg build_function_cfg(const std::vector<DecodedInst> &decoded, uint32_t func_start, uint32_t func_end_excl) {
  FunctionCfg cfg;
  cfg.start = func_start;
  cfg.end = func_end_excl;

  cfg.insts.reserve(decoded.size());
  for (const auto &di : decoded) {
    BundleInst bi;
    bi.inst_pc = di.pc;
    bi.pc = di.had_regext ? di.regext.pc : di.pc;
    bi.len = static_cast<uint8_t>(di.had_regext ? (di.regext.prefix_bytes + 4) : 4);
    bi.inst = di;
    cfg.insts.push_back(std::move(bi));
  }

  require(!cfg.insts.empty(), "CFG: empty function");

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto &bi = cfg.insts[i];
    require(in_range(bi.pc, func_start, func_end_excl), "CFG: instruction pc out of function range");
    const auto [it, ok] = cfg.inst_index_by_pc.emplace(bi.pc, i);
    (void)it;
    require(ok, "CFG: duplicate instruction pc (bundle start)");
  }

  // Leaders
  std::unordered_set<uint32_t> leaders;
  leaders.reserve(cfg.insts.size() / 4);
  leaders.insert(cfg.insts.front().pc);
  leaders.insert(func_start);

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto &bi = cfg.insts[i];
    const auto ci = classify(bi);

    if (bi.inst.name == "join") leaders.insert(bi.pc);

    if ((ci.is_branch || ci.is_jump) && in_range(ci.target, func_start, func_end_excl)) {
      require(cfg.inst_index_by_pc.contains(ci.target), "CFG: missing branch/jump target at pc=" + hex_u32(ci.target));
      leaders.insert(ci.target);
    }
    if (ci.is_terminator && i + 1 < cfg.insts.size()) {
      leaders.insert(cfg.insts[i + 1].pc);
    }
  }

  // Build blocks
  auto flush_block = [&](size_t b_begin, size_t b_end_excl) {
    BasicBlock bb;
    bb.start = cfg.insts[b_begin].pc;
    bb.inst_indices.reserve(b_end_excl - b_begin);
    for (size_t k = b_begin; k < b_end_excl; ++k) {
      bb.inst_indices.push_back(k);
      cfg.inst_pc_to_block[cfg.insts[k].pc] = bb.start;
    }
    cfg.blocks.push_back(std::move(bb));
  };

  size_t cur = 0;
  while (cur < cfg.insts.size()) {
    const size_t begin = cur;
    ++cur;
    for (; cur < cfg.insts.size(); ++cur) {
      const auto &prev = cfg.insts[cur - 1];
      const auto ci_prev = classify(prev);
      if (ci_prev.is_terminator) break;
      if (leaders.contains(cfg.insts[cur].pc)) break;
    }
    flush_block(begin, cur);
    if (cur < cfg.insts.size() && leaders.contains(cfg.insts[cur].pc)) {
      // ok
    }
  }

  // Build edges
  cfg.block_index_by_start.reserve(cfg.blocks.size());
  for (size_t i = 0; i < cfg.blocks.size(); ++i) {
    cfg.block_index_by_start[cfg.blocks[i].start] = i;
  }

  auto dst_block = [&](uint32_t dst_pc) -> uint32_t {
    auto it = cfg.inst_pc_to_block.find(dst_pc);
    if (it == cfg.inst_pc_to_block.end()) throw std::runtime_error("CFG: dst pc not in inst_pc_to_block");
    return it->second;
  };

  for (auto &bb : cfg.blocks) {
    const BundleInst &last = cfg.insts[bb.inst_indices.back()];
    const CfInfo ci = classify(last);

    auto add_edge = [&](uint32_t dst_pc, EdgeKind kind) {
      if (!in_range(dst_pc, func_start, func_end_excl)) return;
      Edge e;
      e.src = bb.start;
      e.dst = dst_block(dst_pc);
      e.kind = kind;
      bb.succs.push_back(e);
    };

    if (ci.is_branch) {
      add_edge(ci.target, EdgeKind::Branch);
      add_edge(last.inst_pc + 4, EdgeKind::Fallthrough);
    } else if (ci.is_jump) {
      add_edge(ci.target, EdgeKind::Jump);
    } else if (ci.is_return || last.inst.name == "endprg" || is_indirect_jalr(last.inst)) {
      // no succ
    } else {
      add_edge(last.inst_pc + 4, EdgeKind::Fallthrough);
    }
  }

  return cfg;
}

} // namespace sbt::cfg
