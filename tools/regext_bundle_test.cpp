#include "sbt/cfg.hpp"
#include "sbt/riscv_decode.hpp"
#include "sbt/spike_encoding_parser.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

static void append_u32_le(std::vector<uint8_t> &out, uint32_t w) {
  out.push_back(uint8_t(w & 0xFFu));
  out.push_back(uint8_t((w >> 8) & 0xFFu));
  out.push_back(uint8_t((w >> 16) & 0xFFu));
  out.push_back(uint8_t((w >> 24) & 0xFFu));
}

static uint32_t set_bits(uint32_t w, int lo, int width, uint32_t v) {
  const uint32_t mask = ((width == 32) ? 0xFFFF'FFFFu : ((1u << width) - 1u)) << lo;
  w &= ~mask;
  w |= (v << lo) & mask;
  return w;
}

struct PatternInfo final {
  std::string id;
  sbt::Pattern p;
};

static void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
}

} // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const fs::path encoding_h = "../spike/riscv/encoding.h";
  const auto decl = sbt::spike::parse_declared_insns(encoding_h);

  std::vector<std::string> name_storage;
  name_storage.reserve(8);
  std::vector<sbt::Pattern> patterns;
  patterns.reserve(8);

  auto add_pat = [&](const std::string &id) {
    auto it = decl.find(id);
    if (it == decl.end()) throw std::runtime_error("missing DECLARE_INSN: " + id);
    name_storage.push_back(id);
    patterns.push_back({name_storage.back().c_str(), it->second.match, it->second.mask});
  };

  add_pat("regext");
  add_pat("regexti");
  add_pat("vadd_vv");
  add_pat("vadd_vi");

  // 1) regext bundling extends registers.
  {
    // imm12 layout (decoder):
    //   ext_rd  = imm12[2:0]
    //   ext_rs1 = imm12[5:3]
    //   ext_rs2 = imm12[8:6]
    //   ext_rs3 = imm12[11:9]
    const uint32_t imm12 = (4u << 9) | (3u << 6) | (2u << 3) | 1u;
    uint32_t w_regext = patterns[0].match;
    w_regext = set_bits(w_regext, 20, 12, imm12);

    uint32_t w_vadd = patterns[2].match;
    w_vadd = set_bits(w_vadd, 7, 5, 1u);   // rd5
    w_vadd = set_bits(w_vadd, 15, 5, 2u);  // rs1_5
    w_vadd = set_bits(w_vadd, 20, 5, 3u);  // rs2_5

    std::vector<uint8_t> text;
    append_u32_le(text, w_regext);
    append_u32_le(text, w_vadd);

    sbt::DecodeOptions opt;
    opt.bundle_regext = true;
    opt.require_known = true;
    const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x1000u, opt, patterns);
    require(decoded.size() == 1, "bundled regext produces one decoded inst");
    require(decoded[0].had_regext, "inst carries regext info");
    require(decoded[0].rd == 33, "rd extended");
    require(decoded[0].rs1 == 66, "rs1 extended");
    require(decoded[0].rs2 == 99, "rs2 extended");
  }

  // 2) regexti bundling extends vi immediates and registers.
  {
    // regexti decoder layout:
    //   ext_rd  = imm12[2:0]
    //   ext_rs2 = imm12[5:3]
    //   ext_imm = imm12[11:6]  (signed 6-bit, applied to 5-bit immediate)
    const uint32_t imm12 = (0x3Fu << 6) | (2u << 3) | 1u; // ext_imm=0x3f -> -1
    uint32_t w_regexti = patterns[1].match;
    w_regexti = set_bits(w_regexti, 20, 12, imm12);

    // vadd.vi encoding: vd (rd5), vs2 (rs2_5), imm5 (rs1_5)
    uint32_t w_vadd_vi = patterns[3].match;
    w_vadd_vi = set_bits(w_vadd_vi, 7, 5, 4u);   // rd5
    w_vadd_vi = set_bits(w_vadd_vi, 20, 5, 5u);  // rs2_5
    w_vadd_vi = set_bits(w_vadd_vi, 15, 5, 1u);  // imm5 (low)

    std::vector<uint8_t> text;
    append_u32_le(text, w_regexti);
    append_u32_le(text, w_vadd_vi);

    sbt::DecodeOptions opt;
    opt.bundle_regext = true;
    opt.require_known = true;
    const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x2000u, opt, patterns);
    require(decoded.size() == 1, "bundled regexti produces one decoded inst");
    require(decoded[0].had_regext, "inst carries regext info");

    // ext_rd=1, ext_rs2=2
    require(decoded[0].rd == (4 | (1 << 5)), "rd extended by regexti");
    require(decoded[0].rs2 == (5 | (2 << 5)), "rs2 extended by regexti");

    // ext_imm = -1 => imm11 = (-1 << 5) + low5(=1) = -31
    require(decoded[0].imm == -31, "imm extended by regexti");
  }

  // 3) nested regext is rejected (edge case).
  {
    uint32_t w_regext = patterns[0].match;
    w_regext = set_bits(w_regext, 20, 12, 1u);
    uint32_t w_vadd = patterns[2].match;
    w_vadd = set_bits(w_vadd, 7, 5, 1u);

    std::vector<uint8_t> text;
    append_u32_le(text, w_regext);
    append_u32_le(text, w_regext);
    append_u32_le(text, w_vadd);

    sbt::DecodeOptions opt;
    opt.bundle_regext = true;
    opt.require_known = true;

    bool threw = false;
    try {
      (void)sbt::decode_text(text, /*text_vaddr=*/0x3000u, opt, patterns);
    } catch (const std::exception &) {
      threw = true;
    }
    require(threw, "nested regext throws");
  }

  // 4) dangling regext at end is rejected (edge case).
  {
    uint32_t w_regext = patterns[0].match;
    w_regext = set_bits(w_regext, 20, 12, 1u);

    std::vector<uint8_t> text;
    append_u32_le(text, w_regext);

    sbt::DecodeOptions opt;
    opt.bundle_regext = true;
    opt.require_known = true;

    bool threw = false;
    try {
      (void)sbt::decode_text(text, /*text_vaddr=*/0x4000u, opt, patterns);
    } catch (const std::exception &) {
      threw = true;
    }
    require(threw, "dangling regext throws");
  }

  // 5) temporary Spike-compat mode accepts nested regexti+regext and follows Spike overwrite semantics.
  {
    const uint32_t regexti_imm12 = (3u << 6) | (2u << 3) | 1u;
    uint32_t w_regexti = patterns[1].match;
    w_regexti = set_bits(w_regexti, 20, 12, regexti_imm12);

    const uint32_t regext_imm12 = (7u << 9) | (6u << 6) | (5u << 3) | 4u;
    uint32_t w_regext = patterns[0].match;
    w_regext = set_bits(w_regext, 20, 12, regext_imm12);

    uint32_t w_vadd_vi = patterns[3].match;
    w_vadd_vi = set_bits(w_vadd_vi, 7, 5, 4u);   // rd5
    w_vadd_vi = set_bits(w_vadd_vi, 20, 5, 5u);  // rs2_5
    w_vadd_vi = set_bits(w_vadd_vi, 15, 5, 1u);  // imm5

    std::vector<uint8_t> text;
    append_u32_le(text, w_regexti);
    append_u32_le(text, w_regext);
    append_u32_le(text, w_vadd_vi);

    sbt::DecodeOptions opt;
    opt.bundle_regext = true;
    opt.require_known = true;
    opt.spike_compat_nested_regext = true;
    const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x5000u, opt, patterns);
    require(decoded.size() == 1, "compat nested prefixes produce one decoded inst");
    require(decoded[0].had_regext, "compat nested prefixes keep regext info");
    require(decoded[0].regext.pc == 0x5000u, "bundle start stays at first prefix");
    require(decoded[0].regext.prefix_bytes == 8, "two prefixes consume 8 bytes");
    require(decoded[0].regext.validi, "regexti validi survives later regext like Spike");
    require(decoded[0].regext.ext_imm == 0, "later regext clears ext_imm like Spike");
    require(decoded[0].rd == (4 | (4 << 5)), "later regext overrides rd extension");
    require(decoded[0].rs2 == (5 | (6 << 5)), "later regext overrides rs2 extension");
    require(decoded[0].imm == 1, "vi immediate follows Spike-compatible ext_imm overwrite");

    const auto cfg = sbt::cfg::build_function_cfg(decoded, /*func_start=*/0x5000u, /*func_end_excl=*/0x500cu);
    require(cfg.insts.size() == 1, "compat cfg contains one bundled inst");
    require(cfg.insts[0].pc == 0x5000u, "cfg bundle start matches first prefix");
    require(cfg.insts[0].inst_pc == 0x5008u, "cfg inst pc points at real instruction");
    require(cfg.insts[0].len == 12, "cfg bundle length covers both prefixes and real instruction");
  }

  // 6) compat mode still rejects prefix chains that would overflow BundleInst.len.
  {
    uint32_t w_regext = patterns[0].match;
    w_regext = set_bits(w_regext, 20, 12, 1u);
    uint32_t w_vadd = patterns[2].match;
    w_vadd = set_bits(w_vadd, 7, 5, 1u);

    std::vector<uint8_t> text;
    for (int i = 0; i < 63; ++i) append_u32_le(text, w_regext);
    append_u32_le(text, w_vadd);

    sbt::DecodeOptions opt;
    opt.bundle_regext = true;
    opt.require_known = true;
    opt.spike_compat_nested_regext = true;

    bool threw = false;
    try {
      (void)sbt::decode_text(text, /*text_vaddr=*/0x6000u, opt, patterns);
    } catch (const std::exception &) {
      threw = true;
    }
    require(threw, "compat mode rejects prefix chains before bundle length wraps");
  }

  std::cout << "ok regext bundling\n";
  return 0;
}
