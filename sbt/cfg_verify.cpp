#include "sbt/builtin_semantics.hpp"
#include "sbt/cfg_verify.hpp"
#include "sbt/control_semantics.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sbt::cfg {
namespace {

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

class BitSet final {
public:
  explicit BitSet(size_t nbits, bool fill)
      : nbits_(nbits), w_((nbits + 63) / 64, fill ? ~0ULL : 0ULL) {
    if (fill)
      trim();
  }

  static BitSet empty(size_t nbits) { return BitSet(nbits, false); }
  static BitSet full(size_t nbits) { return BitSet(nbits, true); }

  bool test(size_t i) const {
    const size_t wi = i / 64;
    const size_t bi = i % 64;
    return (w_[wi] >> bi) & 1ULL;
  }

  void set(size_t i) {
    const size_t wi = i / 64;
    const size_t bi = i % 64;
    w_[wi] |= (1ULL << bi);
  }

  void clear(size_t i) {
    const size_t wi = i / 64;
    const size_t bi = i % 64;
    w_[wi] &= ~(1ULL << bi);
  }

  BitSet &operator&=(const BitSet &o) {
    for (size_t i = 0; i < w_.size(); ++i)
      w_[i] &= o.w_[i];
    return *this;
  }

  bool operator==(const BitSet &o) const { return w_ == o.w_; }
  bool operator!=(const BitSet &o) const { return !(*this == o); }

private:
  void trim() {
    const size_t rem = nbits_ % 64;
    if (rem == 0)
      return;
    const uint64_t mask = (rem == 64) ? ~0ULL : ((1ULL << rem) - 1ULL);
    w_.back() &= mask;
  }

  size_t nbits_ = 0;
  std::vector<uint64_t> w_;
};

static void transfer_vreg_uniform(VRegUniformFacts &st,
                                  const BundleInst &bi) {
  const auto &di = bi.inst;
  if (di.rd_class != sbt::RegClass::V)
    return;
  if (di.uniform_transfer_kind == sbt::UniformTransferKind::Unknown) {
    if (di.custom.valid || di.mma.valid) {
      st.set(di.rd, false);
      return;
    }
    throw std::runtime_error(
        "missing shared uniform-transfer metadata for supported instruction '" +
        di.name + "'");
  }

  const int vd = di.rd;
  bool uniform = false;
  switch (di.uniform_transfer_kind) {
  case sbt::UniformTransferKind::AlwaysUniformDst:
    uniform = true;
    break;
  case sbt::UniformTransferKind::NeverUniformDst:
    uniform = false;
    break;
  case sbt::UniformTransferKind::UniformIfRs1:
    if (di.rs1_class == sbt::RegClass::None || di.rs1 < 0) {
      throw std::runtime_error(
          "incomplete uniform-transfer metadata for instruction '" + di.name +
          "': missing rs1");
    }
    uniform = st.test(di.rs1);
    break;
  case sbt::UniformTransferKind::UniformIfRs2:
    if (di.rs2_class == sbt::RegClass::None || di.rs2 < 0) {
      throw std::runtime_error(
          "incomplete uniform-transfer metadata for instruction '" + di.name +
          "': missing rs2");
    }
    uniform = st.test(di.rs2);
    break;
  case sbt::UniformTransferKind::UniformIfRs1AndRs2:
    if (di.rs1_class == sbt::RegClass::None || di.rs1 < 0 ||
        di.rs2_class == sbt::RegClass::None || di.rs2 < 0) {
      throw std::runtime_error(
          "incomplete uniform-transfer metadata for instruction '" + di.name +
          "': missing rs1/rs2");
    }
    uniform = st.test(di.rs1) && st.test(di.rs2);
    break;
  case sbt::UniformTransferKind::NotApplicable:
    throw std::runtime_error("unexpected non-applicable uniform-transfer "
                             "metadata on vector-dst instruction '" +
                             di.name + "'");
  case sbt::UniformTransferKind::Unknown:
  default:
    throw std::runtime_error(
        "missing shared uniform-transfer metadata for supported instruction '" +
        di.name + "'");
  }

  st.set(vd, uniform);
}

static bool builtin_summary_write_is_uniform(const VRegUniformFacts &pre,
                                             const VectorWriteSummary &write) {
  switch (write.transfer) {
  case BuiltinUniformTransfer::WorkGroupUniform:
    return true;
  case BuiltinUniformTransfer::WorkItemVarying:
    return false;
  case BuiltinUniformTransfer::SameAsInputs:
    for (int src : write.input_vregs) {
      if (!pre.test(src))
        return false;
    }
    return true;
  }
  return false;
}

static void apply_builtin_summary(VRegUniformFacts &st,
                                  const BuiltinSummary &summary) {
  const VRegUniformFacts pre = st;
  for (const auto &write : summary.vector_writes) {
    st.set(write.dst_vreg, builtin_summary_write_is_uniform(pre, write));
  }
}

static void clear_caller_saved_vregs(VRegUniformFacts &st) {
  static constexpr int kFirstCalleeSavedVReg = 32;
  for (int reg = 0; reg < kFirstCalleeSavedVReg; ++reg)
    st.set(reg, false);
}

static void transfer_vreg_uniform(VRegUniformFacts &st, const BundleInst &bi,
                                  const VerifyOptions &options) {
  const auto semantics = sbt::control::classify(bi);
  if (!semantics.is_direct_call) {
    transfer_vreg_uniform(st, bi);
    return;
  }

  if (semantics.direct_target < 0 ||
      semantics.direct_target > 0xffff'ffffll ||
      options.sym_by_addr == nullptr) {
    clear_caller_saved_vregs(st);
    return;
  }

  const uint32_t target = static_cast<uint32_t>(semantics.direct_target);
  const auto sym_it = options.sym_by_addr->find(target);
  if (sym_it == options.sym_by_addr->end()) {
    clear_caller_saved_vregs(st);
    return;
  }

  const std::string &callee = sym_it->second;
  const auto builtin = sbt::lookup_builtin_call(callee);
  if (!builtin) {
    clear_caller_saved_vregs(st);
    return;
  }

  const auto summary = sbt::builtin_summary_for(*builtin);
  if (!summary) {
    throw std::runtime_error("builtin semantic metadata drift for inlined "
                             "builtin '" +
                             callee + "'");
  }
  apply_builtin_summary(st, *summary);
}

struct Graph final {
  std::vector<uint32_t> nodes; // block starts
  std::unordered_map<uint32_t, size_t> idx_of;
  std::vector<std::unordered_set<uint32_t>> succs;
  std::vector<std::unordered_set<uint32_t>> preds;
};

static Graph build_graph(const FunctionCfg &cfg) {
  Graph g;
  g.nodes.reserve(cfg.blocks.size());
  for (const auto &bb : cfg.blocks)
    g.nodes.push_back(bb.start);
  std::sort(g.nodes.begin(), g.nodes.end());
  for (size_t i = 0; i < g.nodes.size(); ++i)
    g.idx_of[g.nodes[i]] = i;

  g.succs.resize(g.nodes.size());
  g.preds.resize(g.nodes.size());

  for (const auto &bb : cfg.blocks) {
    const size_t si = g.idx_of.at(bb.start);
    for (const auto &e : bb.succs) {
      const auto it = g.idx_of.find(e.dst);
      if (it == g.idx_of.end())
        continue;
      g.succs[si].insert(e.dst);
      g.preds[it->second].insert(bb.start);
    }
  }
  return g;
}

static std::vector<BitSet> compute_dominators(const Graph &g, uint32_t entry) {
  const size_t n = g.nodes.size();
  std::vector<BitSet> dom;
  dom.reserve(n);
  for (size_t i = 0; i < n; ++i)
    dom.push_back(BitSet::full(n));

  const size_t entry_i = g.idx_of.at(entry);
  dom[entry_i] = BitSet::empty(n);
  dom[entry_i].set(entry_i);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (i == entry_i)
        continue;

      BitSet newset = BitSet::empty(n);
      const auto &ps = g.preds[i];
      if (!ps.empty()) {
        auto it = ps.begin();
        newset = dom[g.idx_of.at(*it)];
        ++it;
        for (; it != ps.end(); ++it) {
          newset &= dom[g.idx_of.at(*it)];
        }
      }
      newset.set(i);
      if (newset != dom[i]) {
        dom[i] = std::move(newset);
        changed = true;
      }
    }
  }
  return dom;
}

static std::vector<BitSet> compute_postdominators(const Graph &g) {
  const size_t n = g.nodes.size();
  const size_t exit_i = n;
  const size_t n2 = n + 1;

  std::vector<BitSet> postdom;
  postdom.reserve(n2);
  for (size_t i = 0; i < n2; ++i)
    postdom.push_back(BitSet::full(n2));

  postdom[exit_i] = BitSet::empty(n2);
  postdom[exit_i].set(exit_i);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      BitSet inter = BitSet::full(n2);
      const auto &ss = g.succs[i];
      if (ss.empty()) {
        inter = postdom[exit_i];
      } else {
        auto it = ss.begin();
        inter = postdom[g.idx_of.at(*it)];
        ++it;
        for (; it != ss.end(); ++it) {
          inter &= postdom[g.idx_of.at(*it)];
        }
      }
      inter.set(i);
      if (inter != postdom[i]) {
        postdom[i] = std::move(inter);
        changed = true;
      }
    }
  }
  return postdom;
}

static std::vector<char> compute_reachable(const Graph &g, uint32_t entry) {
  const size_t n = g.nodes.size();
  std::vector<char> reach(n, 0);
  const auto it = g.idx_of.find(entry);
  if (it == g.idx_of.end())
    return reach;

  std::queue<size_t> q;
  q.push(it->second);
  reach[it->second] = 1;
  while (!q.empty()) {
    const size_t cur = q.front();
    q.pop();
    for (uint32_t s : g.succs[cur]) {
      const size_t si = g.idx_of.at(s);
      if (reach[si])
        continue;
      reach[si] = 1;
      q.push(si);
    }
  }
  return reach;
}

struct VRegUniformData final {
  std::vector<VRegUniformFacts> in;
  std::vector<VRegUniformFacts> out;
  std::vector<char> reachable;
};

static VRegUniformData compute_vreg_uniform_must(const FunctionCfg &cfg,
                                                 const Graph &g,
                                                 uint32_t entry,
                                                 const VerifyOptions &options) {
  const size_t n = g.nodes.size();
  VRegUniformData d;
  d.in.resize(n, VRegUniformFacts::empty());
  d.out.resize(n, VRegUniformFacts::empty());
  d.reachable = compute_reachable(g, entry);

  const auto it_entry = g.idx_of.find(entry);
  if (it_entry == g.idx_of.end())
    return d;
  const size_t entry_i = it_entry->second;

  for (size_t i = 0; i < n; ++i) {
    if (!d.reachable[i])
      continue;
    if (i == entry_i) {
      d.in[i] = options.entry_uniform_vregs;
      d.out[i] = options.entry_uniform_vregs;
      continue;
    }
    d.in[i] = VRegUniformFacts::full();
    d.out[i] = VRegUniformFacts::full();
  }

  auto transfer_block = [&](uint32_t block_start,
                            const VRegUniformFacts &in_state)
      -> VRegUniformFacts {
    VRegUniformFacts st = in_state;
    const auto it = cfg.block_index_by_start.find(block_start);
    if (it == cfg.block_index_by_start.end())
      return st;
    const auto &bb = cfg.blocks[it->second];
    for (size_t inst_i : bb.inst_indices) {
      transfer_vreg_uniform(st, cfg.insts[inst_i], options);
    }
    return st;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (!d.reachable[i])
        continue;

      VRegUniformFacts new_in = VRegUniformFacts::empty();
      if (i == entry_i) {
        new_in = options.entry_uniform_vregs;
      } else {
        const auto &ps = g.preds[i];
        bool has_reach_pred = false;
        VRegUniformFacts acc = VRegUniformFacts::full();
        for (uint32_t p : ps) {
          const size_t pi = g.idx_of.at(p);
          if (!d.reachable[pi])
            continue;
          if (!has_reach_pred) {
            acc = d.out[pi];
            has_reach_pred = true;
          } else {
            acc &= d.out[pi];
          }
        }
        new_in = has_reach_pred ? acc : VRegUniformFacts::empty();
      }

      const VRegUniformFacts new_out = transfer_block(g.nodes[i], new_in);

      if (new_in != d.in[i] || new_out != d.out[i]) {
        d.in[i] = new_in;
        d.out[i] = new_out;
        changed = true;
      }
    }
  }

  return d;
}

struct VBranchDerived final {
  VBranchCheck check;
  BitSet region;
};

static BitSet compute_region(const Graph &g, size_t vblock_i, size_t join_i) {
  BitSet region = BitSet::empty(g.nodes.size());
  std::queue<size_t> q;

  for (uint32_t s : g.succs[vblock_i]) {
    const size_t si = g.idx_of.at(s);
    if (si == join_i)
      continue;
    if (region.test(si))
      continue;
    region.set(si);
    q.push(si);
  }

  while (!q.empty()) {
    const size_t n = q.front();
    q.pop();
    for (uint32_t s : g.succs[n]) {
      const size_t si = g.idx_of.at(s);
      if (si == join_i)
        continue;
      if (region.test(si))
        continue;
      region.set(si);
      q.push(si);
    }
  }
  return region;
}

static bool is_jump_block(const FunctionCfg &cfg, uint32_t block_start,
                          uint32_t &out_dst) {
  const auto it = cfg.block_index_by_start.find(block_start);
  if (it == cfg.block_index_by_start.end())
    return false;
  const auto &bb = cfg.blocks[it->second];
  if (bb.succs.size() != 1)
    return false;
  if (bb.succs[0].kind != EdgeKind::Jump)
    return false;
  out_dst = bb.succs[0].dst;
  return true;
}

static bool has_converged_sync_in_region(uint32_t sync_block,
                                         std::string_view sync_kind,
                                         const Graph &g,
                                         const std::vector<VBranchDerived> &vbs,
                                         std::string &out_reason) {
  const auto it = g.idx_of.find(sync_block);
  if (it == g.idx_of.end())
    return false;
  const size_t b_i = it->second;
  for (const auto &vb : vbs) {
    if (vb.check.proven_uniform)
      continue;
    if (vb.region.test(b_i)) {
      out_reason = std::string(sync_kind) + " 位于 vbranch@" +
                   hex_u32(vb.check.vbranch_addr) +
                   " 的分支区域内（无法证明收敛）";
      return true;
    }
  }
  return false;
}

static std::optional<std::string> converged_sync_kind_for_inst(
    const BundleInst &bi, const VerifyOptions &options) {
  if (sbt::control::classify(bi).is_barrier)
    return std::string("barrier");

  const auto semantics = sbt::control::classify(bi);
  if (!semantics.is_direct_call || semantics.direct_target < 0 ||
      semantics.direct_target > 0xffff'ffffll ||
      options.sym_by_addr == nullptr) {
    return std::nullopt;
  }

  const auto sym_it =
      options.sym_by_addr->find(static_cast<uint32_t>(semantics.direct_target));
  if (sym_it == options.sym_by_addr->end())
    return std::nullopt;

  const auto builtin = sbt::lookup_builtin_call(sym_it->second);
  if (!builtin)
    return std::nullopt;

  const auto summary = sbt::builtin_summary_for(*builtin);
  if (!summary) {
    throw std::runtime_error("builtin semantic metadata drift for inlined "
                             "builtin '" +
                             sym_it->second + "'");
  }
  if (!summary->requires_converged_work_group)
    return std::nullopt;

  return sym_it->second;
}

static bool block_in_divergent_region(uint32_t block, const Graph &g,
                                      const std::vector<VBranchDerived> &vbs) {
  const auto it = g.idx_of.find(block);
  if (it == g.idx_of.end())
    return false;
  const size_t block_i = it->second;
  for (const auto &vb : vbs) {
    if (vb.check.proven_uniform)
      continue;
    if (vb.region.test(block_i))
      return true;
  }
  return false;
}

struct AnalysisContext final {
  Graph graph;
  std::vector<VBranchDerived> vbs;
  VRegUniformData vuni;
};

static AnalysisContext analyze_function(const FunctionCfg &cfg,
                                        const VerifyOptions &options) {
  AnalysisContext ctx;
  ctx.graph = build_graph(cfg);
  const uint32_t entry =
      cfg.blocks.empty() ? cfg.start : cfg.blocks.front().start;
  const std::vector<BitSet> dom = compute_dominators(ctx.graph, entry);
  const std::vector<BitSet> postdom = compute_postdominators(ctx.graph);
  ctx.vuni = compute_vreg_uniform_must(cfg, ctx.graph, entry, options);

  std::unordered_map<uint32_t, const BundleInst *> inst_by_pc;
  inst_by_pc.reserve(cfg.insts.size());
  for (const auto &bi : cfg.insts)
    inst_by_pc[bi.pc] = &bi;

  std::optional<uint32_t> current_rpc;

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto &bi = cfg.insts[i];
    const auto &di = bi.inst;
    const auto semantics = sbt::control::classify(bi);

    if (semantics.is_setrpc) {
      current_rpc = sbt::control::resolve_setrpc_join_pc(cfg, i);
      continue;
    }

    if (!semantics.is_vector_branch)
      continue;

    VBranchDerived derived{.check = {},
                           .region = BitSet::empty(ctx.graph.nodes.size())};
    auto &c = derived.check;
    c.vbranch_addr = bi.pc;
    c.mnemonic = di.name;

    c.target = static_cast<uint32_t>(semantics.direct_target);
    c.fallthrough = bi.inst_pc + 4;

    auto it_vb = cfg.inst_pc_to_block.find(bi.pc);
    if (it_vb == cfg.inst_pc_to_block.end()) {
      c.error = "vbranch 未映射到基本块";
      ctx.vbs.push_back(std::move(derived));
      continue;
    }
    c.vbranch_block = it_vb->second;

    c.join_pc = current_rpc;
    if (!c.join_pc) {
      c.error = "无法在 vbranch 处解析 CSR_RPC(join PC)";
      ctx.vbs.push_back(std::move(derived));
      continue;
    }

    const uint32_t join_pc = *c.join_pc;
    const auto it_join_inst = inst_by_pc.find(join_pc);
    c.join_is_join_inst =
        (it_join_inst != inst_by_pc.end() &&
         sbt::control::classify(*it_join_inst->second).is_join);
    if (!c.join_is_join_inst) {
      c.error = "join PC=" + hex_u32(join_pc) + " 处不存在 join 指令";
      ctx.vbs.push_back(std::move(derived));
      continue;
    }

    const auto it_join_block = cfg.inst_pc_to_block.find(join_pc);
    if (it_join_block == cfg.inst_pc_to_block.end()) {
      c.error = "join PC=" + hex_u32(join_pc) + " 未映射到基本块";
      ctx.vbs.push_back(std::move(derived));
      continue;
    }
    const uint32_t join_block = it_join_block->second;
    if (join_block != join_pc) {
      c.error = "join PC=" + hex_u32(join_pc) + " 未成为基本块入口";
      ctx.vbs.push_back(std::move(derived));
      continue;
    }

    const auto it_vblock_i = ctx.graph.idx_of.find(c.vbranch_block);
    const auto it_join_i = ctx.graph.idx_of.find(join_block);
    if (it_vblock_i == ctx.graph.idx_of.end() ||
        it_join_i == ctx.graph.idx_of.end()) {
      c.error = "CFG 节点索引缺失";
      ctx.vbs.push_back(std::move(derived));
      continue;
    }
    const size_t vblock_i = it_vblock_i->second;
    const size_t join_i = it_join_i->second;

    c.proven_uniform = false;
    if (ctx.vuni.reachable.size() == ctx.graph.nodes.size() &&
        ctx.vuni.reachable[vblock_i]) {
      VRegUniformFacts st = ctx.vuni.in[vblock_i];
      const auto it_bb = cfg.block_index_by_start.find(c.vbranch_block);
      if (it_bb != cfg.block_index_by_start.end()) {
        const auto &bb = cfg.blocks[it_bb->second];
        for (size_t inst_i : bb.inst_indices) {
          if (inst_i == i)
            break;
          transfer_vreg_uniform(st, cfg.insts[inst_i], options);
        }
        if (di.rs1_class == sbt::RegClass::V &&
            di.rs2_class == sbt::RegClass::V) {
          c.proven_uniform = st.test(di.rs1) && st.test(di.rs2);
        }
      }
    }

    c.loop_like = false;
    for (uint32_t s : ctx.graph.succs[vblock_i]) {
      uint32_t cur = s;
      for (int step = 0; step < 8; ++step) {
        const auto it_cur_i = ctx.graph.idx_of.find(cur);
        if (it_cur_i == ctx.graph.idx_of.end())
          break;
        if (dom[vblock_i].test(it_cur_i->second)) {
          c.loop_like = true;
          break;
        }
        uint32_t jdst = 0;
        if (is_jump_block(cfg, cur, jdst)) {
          cur = jdst;
          continue;
        }
        break;
      }
      if (c.loop_like)
        break;
    }

    c.postdom_ok = postdom[vblock_i].test(join_i);
    derived.region = compute_region(ctx.graph, vblock_i, join_i);

    c.no_side_exit_ok = true;
    for (size_t ni = 0; ni < ctx.graph.nodes.size(); ++ni) {
      if (!derived.region.test(ni))
        continue;
      for (uint32_t s : ctx.graph.succs[ni]) {
        const size_t si = ctx.graph.idx_of.at(s);
        if (si == join_i || derived.region.test(si))
          continue;
        c.no_side_exit_ok = false;
        break;
      }
      if (!c.no_side_exit_ok)
        break;
    }

    if (c.loop_like) {
      c.single_entry_ok = true;
    } else {
      c.single_entry_ok = true;
      for (size_t ni = 0; ni < ctx.graph.nodes.size(); ++ni) {
        if (!derived.region.test(ni))
          continue;
        for (uint32_t p : ctx.graph.preds[ni]) {
          const size_t pi = ctx.graph.idx_of.at(p);
          if (p == c.vbranch_block || derived.region.test(pi))
            continue;
          c.single_entry_ok = false;
          break;
        }
        if (!c.single_entry_ok)
          break;
      }
    }

    if (!c.postdom_ok) {
      c.error = "join 块未后支配 vbranch 块";
    } else if (!c.no_side_exit_ok) {
      c.error = "分支区域存在侧出口(side exit)";
    }

    ctx.vbs.push_back(std::move(derived));
  }

  return ctx;
}

} // namespace

FunctionVerifyResult verify_function(const FunctionCfg &cfg,
                                     std::string func_name,
                                     const VerifyOptions &options) {
  FunctionVerifyResult out;
  out.func = std::move(func_name);
  out.start = cfg.start;
  out.end = cfg.end;
  out.insts = cfg.insts.size();
  out.blocks = cfg.blocks.size();
  for (const auto &bb : cfg.blocks)
    out.edges += bb.succs.size();

  const AnalysisContext analysis = analyze_function(cfg, options);

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto &bi = cfg.insts[i];
    const auto &di = bi.inst;
    const auto semantics = sbt::control::classify(bi);

    if (semantics.is_indirect_terminator) {
      UnsupportedJalr uj;
      uj.addr = bi.pc;
      uj.word = di.word;
      out.unsupported_jalr.push_back(uj);
    }
  }

  // Barrier-like checks (conservative): every PTX work-group synchronization
  // point must not be in any vbranch region, and the function must not be
  // entered from a divergent direct-call context.
  for (const auto &bi : cfg.insts) {
    const auto sync_kind = converged_sync_kind_for_inst(bi, options);
    if (!sync_kind)
      continue;
    BarrierCheck bc;
    bc.barrier_addr = bi.pc;
    bc.kind = *sync_kind;
    auto it = cfg.inst_pc_to_block.find(bi.pc);
    if (it == cfg.inst_pc_to_block.end()) {
      bc.ok = false;
      bc.error = bc.kind + " 未映射到基本块";
      out.barriers.push_back(std::move(bc));
      continue;
    }
    bc.barrier_block = it->second;
    if (!options.entry_converged) {
      bc.ok = false;
      bc.error = bc.kind + " 所在函数入口来自非收敛 direct-call 上下文";
    } else {
      std::string reason;
      if (has_converged_sync_in_region(bc.barrier_block, bc.kind,
                                       analysis.graph, analysis.vbs, reason)) {
        bc.ok = false;
        bc.error = std::move(reason);
      } else {
        bc.ok = true;
      }
    }
    out.barriers.push_back(std::move(bc));
  }

  out.vbranch.reserve(analysis.vbs.size());
  for (const auto &vb : analysis.vbs)
    out.vbranch.push_back(vb.check);

  return out;
}

std::vector<DirectCallFacts>
collect_direct_call_facts(const FunctionCfg &cfg, const VerifyOptions &options) {
  const AnalysisContext analysis = analyze_function(cfg, options);
  std::vector<DirectCallFacts> out;

  for (size_t block_i = 0; block_i < analysis.graph.nodes.size(); ++block_i) {
    if (analysis.vuni.reachable.size() == analysis.graph.nodes.size() &&
        !analysis.vuni.reachable[block_i])
      continue;

    const uint32_t block_start = analysis.graph.nodes[block_i];
    const auto it_bb = cfg.block_index_by_start.find(block_start);
    if (it_bb == cfg.block_index_by_start.end())
      continue;

    const auto &bb = cfg.blocks[it_bb->second];
    VRegUniformFacts st =
        analysis.vuni.in.size() == analysis.graph.nodes.size()
            ? analysis.vuni.in[block_i]
            : VRegUniformFacts::empty();
    const bool converged =
        options.entry_converged &&
        !block_in_divergent_region(block_start, analysis.graph, analysis.vbs);

    for (size_t inst_i : bb.inst_indices) {
      const auto &bi = cfg.insts[inst_i];
      const auto semantics = sbt::control::classify(bi);
      if (semantics.is_direct_call) {
        if (semantics.direct_target >= 0 &&
            semantics.direct_target <= 0xffff'ffffll) {
          DirectCallFacts facts;
          facts.call_addr = bi.pc;
          facts.call_block = block_start;
          facts.callee_addr = static_cast<uint32_t>(semantics.direct_target);
          facts.pre_call_uniform_vregs = st;
          facts.call_context_converged = converged;
          out.push_back(std::move(facts));
        }
      }
      transfer_vreg_uniform(st, bi, options);
    }
  }

  return out;
}

FunctionVerifyResult verify_function(const FunctionCfg &cfg,
                                     std::string func_name) {
  return verify_function(cfg, std::move(func_name), VerifyOptions{});
}

} // namespace sbt::cfg
