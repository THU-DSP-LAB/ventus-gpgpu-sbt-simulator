#include "sbt/cfg_verify.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sbt::cfg {
namespace {

static bool is_vbranch(std::string_view name) {
  return name == "vbeq" || name == "vbne" || name == "vblt" || name == "vbge" || name == "vbltu" || name == "vbgeu";
}

static bool is_ret(const sbt::DecodedInst &di) {
  return di.name == "jalr" && di.rd_class == sbt::RegClass::X && di.rs1_class == sbt::RegClass::X && di.rd == 0 &&
         di.rs1 == 1 && di.imm_kind == sbt::ImmKind::I12 && di.imm == 0;
}

static bool is_indirect_jalr(const sbt::DecodedInst &di) { return di.name == "jalr" && !is_ret(di); }

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

class BitSet final {
public:
  explicit BitSet(size_t nbits, bool fill) : nbits_(nbits), w_((nbits + 63) / 64, fill ? ~0ULL : 0ULL) {
    if (fill) trim();
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
    for (size_t i = 0; i < w_.size(); ++i) w_[i] &= o.w_[i];
    return *this;
  }

  bool operator==(const BitSet &o) const { return w_ == o.w_; }
  bool operator!=(const BitSet &o) const { return !(*this == o); }

private:
  void trim() {
    const size_t rem = nbits_ % 64;
    if (rem == 0) return;
    const uint64_t mask = (rem == 64) ? ~0ULL : ((1ULL << rem) - 1ULL);
    w_.back() &= mask;
  }

  size_t nbits_ = 0;
  std::vector<uint64_t> w_;
};

class VRegSet final {
public:
  static constexpr int kMaxVReg = 256;

  static VRegSet empty() { return VRegSet(false); }
  static VRegSet full() { return VRegSet(true); }

  bool test(int r) const {
    if (r < 0 || r >= kMaxVReg) return false;
    const size_t wi = static_cast<size_t>(r) / 64;
    const size_t bi = static_cast<size_t>(r) % 64;
    return (w_[wi] >> bi) & 1ULL;
  }

  void set(int r, bool v) {
    if (r < 0 || r >= kMaxVReg) return;
    const size_t wi = static_cast<size_t>(r) / 64;
    const size_t bi = static_cast<size_t>(r) % 64;
    if (v) w_[wi] |= (1ULL << bi);
    else w_[wi] &= ~(1ULL << bi);
  }

  VRegSet &operator&=(const VRegSet &o) {
    for (size_t i = 0; i < w_.size(); ++i) w_[i] &= o.w_[i];
    return *this;
  }

  bool operator==(const VRegSet &o) const { return w_ == o.w_; }
  bool operator!=(const VRegSet &o) const { return !(*this == o); }

private:
  explicit VRegSet(bool fill) {
    if (fill) {
      w_.fill(~0ULL);
    } else {
      w_.fill(0ULL);
    }
  }

  std::array<uint64_t, 4> w_{};
};

static bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

static bool ends_with(std::string_view s, std::string_view suf) {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

static bool is_vector_load(std::string_view name) { return name == "vlw12_v" || name == "vlbu12_v" || name == "vlw_v"; }

static bool is_uniform_unsafe_op(std::string_view name) { return starts_with(name, "vmadd") || starts_with(name, "vfmadd"); }

static void transfer_vreg_uniform(VRegSet &st, const BundleInst &bi) {
  const auto &di = bi.inst;
  if (di.rd_class != sbt::RegClass::V) return;

  const int vd = di.rd;
  bool uniform = false;
  const std::string_view name(di.name);

  if (name == "vmv_v_x") {
    uniform = true;
  } else if (name == "vid_v") {
    uniform = false;
  } else if (is_vector_load(name) && di.rs1_class == sbt::RegClass::V) {
    uniform = st.test(di.rs1);
  } else if (is_uniform_unsafe_op(name)) {
    uniform = false;
  } else if (ends_with(name, "_vx")) {
    uniform = st.test(di.rs2);
  } else if (ends_with(name, "_vi")) {
    uniform = st.test(di.rs2);
  } else if (ends_with(name, "_vv")) {
    uniform = st.test(di.rs2) && st.test(di.rs1);
  } else if (ends_with(name, "_v") && di.rs2_class == sbt::RegClass::V) {
    uniform = st.test(di.rs2);
  } else {
    uniform = false;
  }

  st.set(vd, uniform);
}

static std::optional<uint32_t> resolve_setrpc_join_pc(const FunctionCfg &cfg, size_t setrpc_inst_idx) {
  const auto &bi = cfg.insts.at(setrpc_inst_idx);
  const auto &di = bi.inst;
  if (di.name != "setrpc") return std::nullopt;
  if (di.rs1_class != sbt::RegClass::X || di.rs1 < 0 || di.rs1 >= 32) return std::nullopt;

  const int rs1 = di.rs1;
  const int32_t off = di.imm;

  const size_t begin = (setrpc_inst_idx > 12 ? setrpc_inst_idx - 12 : 0);
  for (size_t j = setrpc_inst_idx; j-- > begin;) {
    const auto &pj = cfg.insts[j].inst;
    if (pj.name != "auipc") continue;
    if (pj.rd_class != sbt::RegClass::X || pj.rd != rs1) continue;
    const int32_t imm = pj.imm; // already << 12
    const int64_t base = int64_t(cfg.insts[j].inst_pc) + int64_t(imm);
    const int64_t join = base + int64_t(off);
    return static_cast<uint32_t>(join);
  }
  return std::nullopt;
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
  for (const auto &bb : cfg.blocks) g.nodes.push_back(bb.start);
  std::sort(g.nodes.begin(), g.nodes.end());
  for (size_t i = 0; i < g.nodes.size(); ++i) g.idx_of[g.nodes[i]] = i;

  g.succs.resize(g.nodes.size());
  g.preds.resize(g.nodes.size());

  for (const auto &bb : cfg.blocks) {
    const size_t si = g.idx_of.at(bb.start);
    for (const auto &e : bb.succs) {
      const auto it = g.idx_of.find(e.dst);
      if (it == g.idx_of.end()) continue;
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
  for (size_t i = 0; i < n; ++i) dom.push_back(BitSet::full(n));

  const size_t entry_i = g.idx_of.at(entry);
  dom[entry_i] = BitSet::empty(n);
  dom[entry_i].set(entry_i);

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (i == entry_i) continue;

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
  for (size_t i = 0; i < n2; ++i) postdom.push_back(BitSet::full(n2));

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
  if (it == g.idx_of.end()) return reach;

  std::queue<size_t> q;
  q.push(it->second);
  reach[it->second] = 1;
  while (!q.empty()) {
    const size_t cur = q.front();
    q.pop();
    for (uint32_t s : g.succs[cur]) {
      const size_t si = g.idx_of.at(s);
      if (reach[si]) continue;
      reach[si] = 1;
      q.push(si);
    }
  }
  return reach;
}

struct VRegUniformData final {
  std::vector<VRegSet> in;
  std::vector<VRegSet> out;
  std::vector<char> reachable;
};

static VRegUniformData compute_vreg_uniform_must(const FunctionCfg &cfg, const Graph &g, uint32_t entry) {
  const size_t n = g.nodes.size();
  VRegUniformData d;
  d.in.resize(n, VRegSet::empty());
  d.out.resize(n, VRegSet::empty());
  d.reachable = compute_reachable(g, entry);

  const auto it_entry = g.idx_of.find(entry);
  if (it_entry == g.idx_of.end()) return d;
  const size_t entry_i = it_entry->second;

  for (size_t i = 0; i < n; ++i) {
    if (!d.reachable[i]) continue;
    if (i == entry_i) {
      d.in[i] = VRegSet::empty();
      d.out[i] = VRegSet::empty();
      continue;
    }
    d.in[i] = VRegSet::full();
    d.out[i] = VRegSet::full();
  }

  auto transfer_block = [&](uint32_t block_start, const VRegSet &in_state) -> VRegSet {
    VRegSet st = in_state;
    const auto it = cfg.block_index_by_start.find(block_start);
    if (it == cfg.block_index_by_start.end()) return st;
    const auto &bb = cfg.blocks[it->second];
    for (size_t inst_i : bb.inst_indices) {
      transfer_vreg_uniform(st, cfg.insts[inst_i]);
    }
    return st;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (!d.reachable[i]) continue;

      VRegSet new_in = VRegSet::empty();
      if (i == entry_i) {
        new_in = VRegSet::empty();
      } else {
        const auto &ps = g.preds[i];
        bool has_reach_pred = false;
        VRegSet acc = VRegSet::full();
        for (uint32_t p : ps) {
          const size_t pi = g.idx_of.at(p);
          if (!d.reachable[pi]) continue;
          if (!has_reach_pred) {
            acc = d.out[pi];
            has_reach_pred = true;
          } else {
            acc &= d.out[pi];
          }
        }
        new_in = has_reach_pred ? acc : VRegSet::empty();
      }

      const VRegSet new_out = transfer_block(g.nodes[i], new_in);

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
    if (si == join_i) continue;
    if (region.test(si)) continue;
    region.set(si);
    q.push(si);
  }

  while (!q.empty()) {
    const size_t n = q.front();
    q.pop();
    for (uint32_t s : g.succs[n]) {
      const size_t si = g.idx_of.at(s);
      if (si == join_i) continue;
      if (region.test(si)) continue;
      region.set(si);
      q.push(si);
    }
  }
  return region;
}

static bool is_jump_block(const FunctionCfg &cfg, uint32_t block_start, uint32_t &out_dst) {
  const auto it = cfg.block_index_by_start.find(block_start);
  if (it == cfg.block_index_by_start.end()) return false;
  const auto &bb = cfg.blocks[it->second];
  if (bb.succs.size() != 1) return false;
  if (bb.succs[0].kind != EdgeKind::Jump) return false;
  out_dst = bb.succs[0].dst;
  return true;
}

static bool has_barrier_in_region(uint32_t barrier_block, const Graph &g, const std::vector<VBranchDerived> &vbs, std::string &out_reason) {
  const auto it = g.idx_of.find(barrier_block);
  if (it == g.idx_of.end()) return false;
  const size_t b_i = it->second;
  for (const auto &vb : vbs) {
    if (vb.check.proven_uniform) continue;
    if (vb.region.test(b_i)) {
      out_reason = "barrier 位于 vbranch@" + hex_u32(vb.check.vbranch_addr) + " 的分支区域内（无法证明收敛）";
      return true;
    }
  }
  return false;
}

} // namespace

FunctionVerifyResult verify_function(const FunctionCfg &cfg, std::string func_name) {
  FunctionVerifyResult out;
  out.func = std::move(func_name);
  out.start = cfg.start;
  out.end = cfg.end;
  out.insts = cfg.insts.size();
  out.blocks = cfg.blocks.size();
  for (const auto &bb : cfg.blocks) out.edges += bb.succs.size();

  const Graph g = build_graph(cfg);
  const uint32_t entry = cfg.blocks.empty() ? cfg.start : cfg.blocks.front().start;
  const std::vector<BitSet> dom = compute_dominators(g, entry);
  const std::vector<BitSet> postdom = compute_postdominators(g);
  const VRegUniformData vuni = compute_vreg_uniform_must(cfg, g, entry);

  std::unordered_map<uint32_t, const BundleInst *> inst_by_pc;
  inst_by_pc.reserve(cfg.insts.size());
  for (const auto &bi : cfg.insts) inst_by_pc[bi.pc] = &bi;

  std::optional<uint32_t> current_rpc;
  std::vector<VBranchDerived> vbs;

  for (size_t i = 0; i < cfg.insts.size(); ++i) {
    const auto &bi = cfg.insts[i];
    const auto &di = bi.inst;

    if (di.name == "setrpc") {
      current_rpc = resolve_setrpc_join_pc(cfg, i);
      continue;
    }

    if (is_indirect_jalr(di)) {
      UnsupportedJalr uj;
      uj.addr = bi.pc;
      uj.word = di.word;
      out.unsupported_jalr.push_back(uj);
    }

    if (!is_vbranch(di.name)) continue;

    VBranchDerived derived{.check = {}, .region = BitSet::empty(g.nodes.size())};
    auto &c = derived.check;
    c.vbranch_addr = bi.pc;
    c.mnemonic = di.name;

    // target/fallthrough computed from the actual instruction PC.
    if (di.imm_kind == sbt::ImmKind::B13) {
      const int64_t t = int64_t(bi.inst_pc) + int64_t(di.imm);
      c.target = static_cast<uint32_t>(t);
    }
    c.fallthrough = bi.inst_pc + 4;

    auto it_vb = cfg.inst_pc_to_block.find(bi.pc);
    if (it_vb == cfg.inst_pc_to_block.end()) {
      c.error = "vbranch 未映射到基本块";
      vbs.push_back(std::move(derived));
      continue;
    }
    c.vbranch_block = it_vb->second;

    c.join_pc = current_rpc;
    if (!c.join_pc) {
      c.error = "无法在 vbranch 处解析 CSR_RPC(join PC)";
      vbs.push_back(std::move(derived));
      continue;
    }

    const uint32_t join_pc = *c.join_pc;
    const auto it_join_inst = inst_by_pc.find(join_pc);
    c.join_is_join_inst = (it_join_inst != inst_by_pc.end() && it_join_inst->second->inst.name == "join");
    if (!c.join_is_join_inst) {
      c.error = "join PC=" + hex_u32(join_pc) + " 处不存在 join 指令";
      vbs.push_back(std::move(derived));
      continue;
    }

    const auto it_join_block = cfg.inst_pc_to_block.find(join_pc);
    if (it_join_block == cfg.inst_pc_to_block.end()) {
      c.error = "join PC=" + hex_u32(join_pc) + " 未映射到基本块";
      vbs.push_back(std::move(derived));
      continue;
    }
    const uint32_t join_block = it_join_block->second;
    if (join_block != join_pc) {
      c.error = "join PC=" + hex_u32(join_pc) + " 未成为基本块入口";
      vbs.push_back(std::move(derived));
      continue;
    }

    const auto it_vblock_i = g.idx_of.find(c.vbranch_block);
    const auto it_join_i = g.idx_of.find(join_block);
    if (it_vblock_i == g.idx_of.end() || it_join_i == g.idx_of.end()) {
      c.error = "CFG 节点索引缺失";
      vbs.push_back(std::move(derived));
      continue;
    }
    const size_t vblock_i = it_vblock_i->second;
    const size_t join_i = it_join_i->second;

    // Prove vbranch warp-uniform by checking operand vectors are uniform.
    c.proven_uniform = false;
    if (vuni.reachable.size() == g.nodes.size() && vuni.reachable[vblock_i]) {
      VRegSet st = vuni.in[vblock_i];
      const auto it_bb = cfg.block_index_by_start.find(c.vbranch_block);
      if (it_bb != cfg.block_index_by_start.end()) {
        const auto &bb = cfg.blocks[it_bb->second];
        for (size_t inst_i : bb.inst_indices) {
          if (inst_i == i) break;
          transfer_vreg_uniform(st, cfg.insts[inst_i]);
        }
        if (di.rs1_class == sbt::RegClass::V && di.rs2_class == sbt::RegClass::V) {
          c.proven_uniform = st.test(di.rs1) && st.test(di.rs2);
        }
      }
    }

    // loop-like detection (same heuristic as feasibility script).
    c.loop_like = false;
    for (uint32_t s : g.succs[vblock_i]) {
      uint32_t cur = s;
      for (int step = 0; step < 8; ++step) {
        const auto it_cur_i = g.idx_of.find(cur);
        if (it_cur_i == g.idx_of.end()) break;
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
      if (c.loop_like) break;
    }

    c.postdom_ok = postdom[vblock_i].test(join_i);

    derived.region = compute_region(g, vblock_i, join_i);

    c.no_side_exit_ok = true;
    for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
      if (!derived.region.test(ni)) continue;
      for (uint32_t s : g.succs[ni]) {
        const size_t si = g.idx_of.at(s);
        if (si == join_i || derived.region.test(si)) continue;
        c.no_side_exit_ok = false;
        break;
      }
      if (!c.no_side_exit_ok) break;
    }

    if (c.loop_like) {
      c.single_entry_ok = true;
    } else {
      c.single_entry_ok = true;
      for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
        if (!derived.region.test(ni)) continue;
        for (uint32_t p : g.preds[ni]) {
          const size_t pi = g.idx_of.at(p);
          if (p == c.vbranch_block || derived.region.test(pi)) continue;
          c.single_entry_ok = false;
          break;
        }
        if (!c.single_entry_ok) break;
      }
    }

    if (!c.postdom_ok) {
      c.error = "join 块未后支配 vbranch 块";
    } else if (!c.no_side_exit_ok) {
      c.error = "分支区域存在侧出口(side exit)";
    } else if ((!c.loop_like) && (!c.single_entry_ok)) {
      c.error = "分支区域存在多入口(multi-entry)";
    }

    vbs.push_back(std::move(derived));
  }

  // Barrier checks (conservative): barrier block must not be in any vbranch region.
  for (const auto &bi : cfg.insts) {
    if (bi.inst.name != "barrier") continue;
    BarrierCheck bc;
    bc.barrier_addr = bi.pc;
    auto it = cfg.inst_pc_to_block.find(bi.pc);
    if (it == cfg.inst_pc_to_block.end()) {
      bc.ok = false;
      bc.error = "barrier 未映射到基本块";
      out.barriers.push_back(std::move(bc));
      continue;
    }
    bc.barrier_block = it->second;
    std::string reason;
    if (has_barrier_in_region(bc.barrier_block, g, vbs, reason)) {
      bc.ok = false;
      bc.error = std::move(reason);
    } else {
      bc.ok = true;
    }
    out.barriers.push_back(std::move(bc));
  }

  // Move vbranch checks.
  out.vbranch.reserve(vbs.size());
  for (auto &vb : vbs) out.vbranch.push_back(std::move(vb.check));

  return out;
}

} // namespace sbt::cfg
