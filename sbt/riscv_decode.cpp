#include "sbt/riscv_decode.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace sbt {
namespace {

static uint32_t read_u32_le(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static int32_t sext(uint32_t x, int bits) {
  const uint32_t m = 1u << (bits - 1);
  const uint32_t mask = (bits == 32) ? 0xFFFF'FFFFu : ((1u << bits) - 1);
  x &= mask;
  return (x ^ m) - int32_t(m);
}

static uint32_t imm_i(uint32_t w) { return (w >> 20) & 0xFFFu; }
static uint32_t imm_s(uint32_t w) { return ((w >> 25) << 5) | ((w >> 7) & 0x1Fu); }
static uint32_t imm_b(uint32_t w) {
  const uint32_t bit12 = (w >> 31) & 1u;
  const uint32_t bit11 = (w >> 7) & 1u;
  const uint32_t bits10_5 = (w >> 25) & 0x3Fu;
  const uint32_t bits4_1 = (w >> 8) & 0xFu;
  return (bit12 << 12) | (bit11 << 11) | (bits10_5 << 5) | (bits4_1 << 1);
}
static uint32_t imm_u(uint32_t w) { return w & 0xFFFFF000u; }
static uint32_t imm_j(uint32_t w) {
  const uint32_t bit20 = (w >> 31) & 1u;
  const uint32_t bits10_1 = (w >> 21) & 0x3FFu;
  const uint32_t bit11 = (w >> 20) & 1u;
  const uint32_t bits19_12 = (w >> 12) & 0xFFu;
  return (bit20 << 20) | (bits19_12 << 12) | (bit11 << 11) | (bits10_1 << 1);
}

static bool is_vbranch(std::string_view name) {
  return name == "vbeq" || name == "vbne" || name == "vblt" || name == "vbge" || name == "vbltu" ||
         name == "vbgeu";
}

static bool is_load(std::string_view name) {
  return name == "vlw12_v" || name == "vlbu12_v" || name == "vlw_v";
}

static bool is_store(std::string_view name) { return name == "vsw12_v" || name == "vsb12_v" || name == "vsw_v"; }

static bool ends_with(std::string_view s, std::string_view suf) {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

static int ext_apply(int base5, uint8_t ext3) { return base5 | (int(ext3) << 5); }

static Pattern const *match_pattern(uint32_t w, const std::vector<Pattern> &patterns) {
  for (const auto &p : patterns) {
    if (p.name == nullptr) continue;
    if ((w & p.mask) == p.match) return &p;
  }
  return nullptr;
}

static void classify_by_name(std::string_view name, DecodedInst &out) {
  // Default: no operands/imm.
  out.rd_class = RegClass::None;
  out.rs1_class = RegClass::None;
  out.rs2_class = RegClass::None;
  out.rs3_class = RegClass::None;
  out.imm_kind = ImmKind::None;
  out.imm = 0;

  if (name == "setrpc") {
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.imm_kind = ImmKind::I12;
    return;
  }
  if (name == "barrier") {
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.imm_kind = ImmKind::I12;
    return;
  }
  if (name == "join" || name == "endprg") {
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
    return;
  }
  if (is_vbranch(name)) {
    out.rs1_class = RegClass::V;
    out.rs2_class = RegClass::V;
    out.imm_kind = ImmKind::B13;
    return;
  }
  if (is_load(name)) {
    out.rd_class = RegClass::V;
    out.rs1_class = RegClass::V;
    out.imm_kind = ImmKind::I12;
    return;
  }
  if (is_store(name)) {
    out.rs2_class = RegClass::V;
    out.rs1_class = RegClass::V;
    out.imm_kind = ImmKind::S12;
    return;
  }
  if (name == "vsetvli") {
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.imm_kind = ImmKind::Raw12;
    return;
  }
  if (name == "vid_v") {
    out.rd_class = RegClass::V;
    return;
  }
  if (name == "vmv_v_x") {
    out.rd_class = RegClass::V;
    out.rs1_class = RegClass::X;
    return;
  }
  if (name == "vadd12_vi" || name == "vsub12_vi") {
    // Custom v*.vi with 12-bit immediate in I-type encoding: vd, vs1, imm12.
    out.rd_class = RegClass::V;
    out.rs1_class = RegClass::V;
    out.imm_kind = ImmKind::I12;
    return;
  }

  // Vector arithmetic common forms inferred from suffix.
  if (ends_with(name, "_vv")) {
    out.rd_class = RegClass::V;
    out.rs2_class = RegClass::V;
    out.rs1_class = RegClass::V;
    return;
  }
  if (ends_with(name, "_vx")) {
    out.rd_class = RegClass::V;
    out.rs2_class = RegClass::V;
    out.rs1_class = RegClass::X;
    return;
  }
  if (ends_with(name, "_vi")) {
    out.rd_class = RegClass::V;
    out.rs2_class = RegClass::V;
    out.imm_kind = (name.find("sll") != std::string_view::npos || name.find("srl") != std::string_view::npos ||
                        name.find("sra") != std::string_view::npos
                    ? ImmKind::UImm5
                    : ImmKind::SImm5);
    return;
  }
  if (ends_with(name, "_v")) {
    // Unary vector op (e.g. vfsqrt.v). Treat as vd, vs2.
    out.rd_class = RegClass::V;
    out.rs2_class = RegClass::V;
    return;
  }
}

static bool decode_scalar(uint32_t w, DecodedInst &out) {
  const uint32_t opcode = w & 0x7Fu;
  const uint32_t rd5 = (w >> 7) & 0x1Fu;
  const uint32_t funct3 = (w >> 12) & 0x7u;
  const uint32_t rs1_5 = (w >> 15) & 0x1Fu;
  const uint32_t rs2_5 = (w >> 20) & 0x1Fu;
  const uint32_t funct7 = (w >> 25) & 0x7Fu;

  auto set_r = [&](std::string n) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
  };
  auto set_i = [&](std::string n, ImmKind k = ImmKind::I12) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.rs1_class = RegClass::X;
    out.imm_kind = k;
  };
  auto set_s = [&](std::string n) {
    out.name = std::move(n);
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
    out.imm_kind = ImmKind::S12;
  };
  auto set_b = [&](std::string n) {
    out.name = std::move(n);
    out.rs1_class = RegClass::X;
    out.rs2_class = RegClass::X;
    out.imm_kind = ImmKind::B13;
  };
  auto set_u = [&](std::string n) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.imm_kind = ImmKind::U20;
  };
  auto set_j = [&](std::string n) {
    out.name = std::move(n);
    out.rd_class = RegClass::X;
    out.imm_kind = ImmKind::J21;
  };

  switch (opcode) {
  case 0x37: // LUI
    set_u("lui");
    out.rd = int(rd5);
    out.imm = int32_t(imm_u(w));
    return true;
  case 0x17: // AUIPC
    set_u("auipc");
    out.rd = int(rd5);
    out.imm = int32_t(imm_u(w));
    return true;
  case 0x6F: // JAL
    set_j("jal");
    out.rd = int(rd5);
    out.imm = sext(imm_j(w), 21);
    return true;
  case 0x67: // JALR
    if (funct3 != 0) return false;
    set_i("jalr");
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.imm = sext(imm_i(w), 12);
    return true;
  case 0x63: // BRANCH
    switch (funct3) {
    case 0x0: set_b("beq"); break;
    case 0x1: set_b("bne"); break;
    case 0x4: set_b("blt"); break;
    case 0x5: set_b("bge"); break;
    case 0x6: set_b("bltu"); break;
    case 0x7: set_b("bgeu"); break;
    default: return false;
    }
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    out.imm = sext(imm_b(w), 13);
    return true;
  case 0x03: // LOAD
    switch (funct3) {
    case 0x0: set_i("lb"); break;
    case 0x1: set_i("lh"); break;
    case 0x2: set_i("lw"); break;
    case 0x4: set_i("lbu"); break;
    case 0x5: set_i("lhu"); break;
    default: return false;
    }
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.imm = sext(imm_i(w), 12);
    return true;
  case 0x23: // STORE
    switch (funct3) {
    case 0x0: set_s("sb"); break;
    case 0x1: set_s("sh"); break;
    case 0x2: set_s("sw"); break;
    default: return false;
    }
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    out.imm = sext(imm_s(w), 12);
    return true;
  case 0x13: // OP-IMM
    switch (funct3) {
    case 0x0: set_i("addi"); out.imm = sext(imm_i(w), 12); break;
    case 0x2: set_i("slti"); out.imm = sext(imm_i(w), 12); break;
    case 0x3: set_i("sltiu"); out.imm = sext(imm_i(w), 12); break;
    case 0x4: set_i("xori"); out.imm = sext(imm_i(w), 12); break;
    case 0x6: set_i("ori"); out.imm = sext(imm_i(w), 12); break;
    case 0x7: set_i("andi"); out.imm = sext(imm_i(w), 12); break;
    case 0x1: // SLLI
      if (funct7 != 0x00) return false;
      set_i("slli");
      out.imm = int32_t((w >> 20) & 0x1Fu);
      break;
    case 0x5: // SRLI/SRAI
      if (funct7 == 0x00) {
        set_i("srli");
      } else if (funct7 == 0x20) {
        set_i("srai");
      } else {
        return false;
      }
      out.imm = int32_t((w >> 20) & 0x1Fu);
      break;
    default: return false;
    }
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    return true;
  case 0x33: // OP
    if (funct7 == 0x01) { // M extension
      switch (funct3) {
      case 0x0: set_r("mul"); break;
      case 0x1: set_r("mulh"); break;
      case 0x2: set_r("mulhsu"); break;
      case 0x3: set_r("mulhu"); break;
      case 0x4: set_r("div"); break;
      case 0x5: set_r("divu"); break;
      case 0x6: set_r("rem"); break;
      case 0x7: set_r("remu"); break;
      default: return false;
      }
    } else {
      switch (funct3) {
      case 0x0:
        if (funct7 == 0x00) set_r("add");
        else if (funct7 == 0x20) set_r("sub");
        else return false;
        break;
      case 0x1: set_r("sll"); break;
      case 0x2: set_r("slt"); break;
      case 0x3: set_r("sltu"); break;
      case 0x4: set_r("xor"); break;
      case 0x5:
        if (funct7 == 0x00) set_r("srl");
        else if (funct7 == 0x20) set_r("sra");
        else return false;
        break;
      case 0x6: set_r("or"); break;
      case 0x7: set_r("and"); break;
      default: return false;
      }
    }
    out.rd = int(rd5);
    out.rs1 = int(rs1_5);
    out.rs2 = int(rs2_5);
    return true;
  case 0x73: // SYSTEM (CSR)
    if (funct3 == 0) {
      // ecall/ebreak/... ignored in prototype
      out.name = "system";
      return true;
    }
    out.rd_class = RegClass::X;
    out.rs1_class = (funct3 >= 0x5 ? RegClass::None : RegClass::X);
    out.imm_kind = ImmKind::CSR12;
    out.imm = int32_t((w >> 20) & 0xFFFu);
    switch (funct3) {
    case 0x1: out.name = "csrrw"; break;
    case 0x2: out.name = "csrrs"; break;
    case 0x3: out.name = "csrrc"; break;
    case 0x5: out.name = "csrrwi"; break;
    case 0x6: out.name = "csrrsi"; break;
    case 0x7: out.name = "csrrci"; break;
    default: out.name = "csr"; break;
    }
    out.rd = int(rd5);
    if (out.rs1_class == RegClass::X) out.rs1 = int(rs1_5);
    return true;
  default: return false;
  }
}

static DecodedInst decode_one(uint32_t pc, uint32_t w, const std::vector<Pattern> &patterns, const RegextPrefix &px) {
  DecodedInst out;
  out.pc = pc;
  out.word = w;
  out.name = "unknown";

  // Default raw fields
  const int rd5 = int((w >> 7) & 0x1F);
  const int rs1_5 = int((w >> 15) & 0x1F);
  const int rs2_5 = int((w >> 20) & 0x1F);
  const int rs3_5 = int((w >> 27) & 0x1F);

  if (const Pattern *p = match_pattern(w, patterns)) {
    out.name = p->name;
    classify_by_name(out.name, out);
    // Fill operands/imm based on classification.
    if (out.rd_class != RegClass::None) out.rd = rd5;
    if (out.rs1_class != RegClass::None) out.rs1 = rs1_5;
    if (out.rs2_class != RegClass::None) out.rs2 = rs2_5;
    if (out.rs3_class != RegClass::None) out.rs3 = rs3_5;

    switch (out.imm_kind) {
    case ImmKind::I12:
      // Some Ventus custom ops reuse I-type encoding but with a non-standard immediate width.
      // `vlw.v` encodes an 11-bit signed offset in bits [30:20] (llvm-objdump prints e.g. 0x7FC as -4).
      if (out.name == "vlw_v") out.imm = sext((w >> 20) & 0x7FFu, 11);
      else out.imm = sext(imm_i(w), 12);
      break;
    case ImmKind::S12: out.imm = sext(imm_s(w), 12); break;
    case ImmKind::B13: out.imm = sext(imm_b(w), 13); break;
    case ImmKind::U20: out.imm = int32_t(imm_u(w)); break;
    case ImmKind::J21: out.imm = sext(imm_j(w), 21); break;
    case ImmKind::CSR12: out.imm = int32_t((w >> 20) & 0xFFFu); break;
    case ImmKind::Raw12: out.imm = int32_t((w >> 20) & 0xFFFu); break;
    case ImmKind::UImm5: out.imm = int32_t((w >> 15) & 0x1Fu); break;
    case ImmKind::SImm5: out.imm = sext((w >> 15) & 0x1Fu, 5); break;
    case ImmKind::None: default: break;
    }
  } else {
    (void)decode_scalar(w, out);
  }

  // Apply regext prefix if present.
  if (px.valid) {
    out.had_regext = true;
    out.regext = px;
    if (out.rd_class != RegClass::None && out.rd >= 0) out.rd = ext_apply(out.rd, px.ext_rd);
    if (out.rs1_class != RegClass::None && out.rs1 >= 0) out.rs1 = ext_apply(out.rs1, px.ext_rs1);
    if (out.rs2_class != RegClass::None && out.rs2 >= 0) out.rs2 = ext_apply(out.rs2, px.ext_rs2);
    if (out.rs3_class != RegClass::None && out.rs3 >= 0) out.rs3 = ext_apply(out.rs3, px.ext_rs3);
  }

  return out;
}

} // namespace

std::vector<DecodedInst> decode_text(const std::vector<uint8_t> &text, uint32_t text_vaddr, const DecodeOptions &opt,
                                     const std::vector<Pattern> &patterns_in) {
  if (text.size() % 4 != 0) throw std::runtime_error(".text size is not multiple of 4");

  std::vector<Pattern> patterns = patterns_in;
  std::sort(patterns.begin(), patterns.end(), [](const Pattern &a, const Pattern &b) {
    const int pa = std::popcount(a.mask);
    const int pb = std::popcount(b.mask);
    if (pa != pb) return pa > pb;
    return std::string_view(a.name) < std::string_view(b.name);
  });

  std::vector<DecodedInst> out;
  out.reserve(text.size() / 4);

  RegextPrefix px{};

  for (size_t off = 0; off < text.size(); off += 4) {
    const uint32_t pc = text_vaddr + static_cast<uint32_t>(off);
    const uint32_t w = read_u32_le(text.data() + off);

    // Handle regext prefix.
    if (opt.bundle_regext) {
      const Pattern *p = match_pattern(w, patterns);
      if (p && std::string_view(p->name) == "regext") {
        if (px.valid) throw std::runtime_error("nested regext prefix at pc=0x" + std::to_string(pc));
        const uint16_t imm12 = static_cast<uint16_t>((w >> 20) & 0xFFFu);
        px.valid = true;
        px.pc = pc;
        px.imm12 = imm12;
        px.ext_rd = imm12 & 7u;
        px.ext_rs1 = (imm12 >> 3) & 7u;
        px.ext_rs2 = (imm12 >> 6) & 7u;
        px.ext_rs3 = (imm12 >> 9) & 7u;
        continue;
      }
      if (p && std::string_view(p->name) == "regexti") {
        throw std::runtime_error("unsupported: regexti at pc=0x" + std::to_string(pc));
      }
    }

    DecodedInst di = decode_one(pc, w, patterns, px);
    if (opt.require_known && di.name == "unknown") {
      throw std::runtime_error("unknown instruction at pc=0x" + std::to_string(pc));
    }
    out.push_back(std::move(di));
    px = RegextPrefix{};
  }

  if (px.valid) {
    throw std::runtime_error("dangling regext prefix at pc=0x" + std::to_string(px.pc));
  }

  return out;
}

const char *to_string(RegClass c) {
  switch (c) {
  case RegClass::X: return "x";
  case RegClass::V: return "v";
  case RegClass::None: default: return "none";
  }
}

const char *to_string(ImmKind k) {
  switch (k) {
  case ImmKind::I12: return "i12";
  case ImmKind::S12: return "s12";
  case ImmKind::B13: return "b13";
  case ImmKind::U20: return "u20";
  case ImmKind::J21: return "j21";
  case ImmKind::CSR12: return "csr12";
  case ImmKind::UImm5: return "uimm5";
  case ImmKind::SImm5: return "simm5";
  case ImmKind::Raw12: return "raw12";
  case ImmKind::None: default: return "none";
  }
}

} // namespace sbt
