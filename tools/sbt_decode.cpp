#include "sbt/dump_parser.hpp"
#include "sbt/elf_reader.hpp"
#include "sbt/cfg.hpp"
#include "sbt/cfg_verify.hpp"
#include "sbt/riscv_decode.hpp"
#include "spike_encoding_subset.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

static void usage() {
  std::cerr << "用法:\n";
  std::cerr << "  sbt_decode funcs  <elf>\n";
  std::cerr << "  sbt_decode decode <elf> [--dump <dump>] [--func <name>] [--json <path>]\n";
  std::cerr << "                  [--require-known] [--no-bundle-regext]\n";
  std::cerr << "  sbt_decode pretty <elf> [--func <name>] [--require-known] [--no-bundle-regext]\n";
  std::cerr << "  sbt_decode cfgverify <elf> [--func <name>] [--json <path>]\n";
  std::cerr << "                     [--include-start] [--verbose] [--require-known] [--no-bundle-regext]\n";
  std::cerr << "  sbt_decode verify <elf> [--dump <dump>]\n";
}

static std::string hex_u32(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", x);
  return std::string(buf);
}

static std::string hex8(uint32_t x) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", x);
  return std::string(buf);
}

static std::string bytes_u32_le(uint32_t w) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02x %02x %02x %02x", unsigned(w & 0xFFu), unsigned((w >> 8) & 0xFFu),
                unsigned((w >> 16) & 0xFFu), unsigned((w >> 24) & 0xFFu));
  return std::string(buf);
}

static std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\\\""; break;
    case '\n': out += "\\\\n"; break;
    case '\r': out += "\\\\r"; break;
    case '\t': out += "\\\\t"; break;
    default:
      if (c < 0x20) {
        char b[8];
        std::snprintf(b, sizeof(b), "\\\\u%04x", unsigned(c));
        out += b;
      } else {
        out.push_back(char(c));
      }
      break;
    }
  }
  return out;
}

static std::vector<sbt::Pattern> build_patterns_from_subset_header() {
  std::vector<sbt::Pattern> out;
  out.reserve(sizeof(sbt::gen::kPatterns) / sizeof(sbt::gen::kPatterns[0]));
  for (const auto &p : sbt::gen::kPatterns) {
    out.push_back({p.name, p.match, p.mask});
  }
  return out;
}

static std::string pretty_mnemonic(std::string_view name) {
  // spike encoding.h uses '_' naming; objdump typically prints '.'.
  std::string out(name);
  for (char &c : out) {
    if (c == '_') c = '.';
  }
  return out;
}

static std::string fmt_reg(sbt::RegClass cls, int idx) {
  if (idx < 0) return "?";
  switch (cls) {
  case sbt::RegClass::X: return "x" + std::to_string(idx);
  case sbt::RegClass::V: return "v" + std::to_string(idx);
  case sbt::RegClass::None: default: return "?";
  }
}

static bool ends_with(std::string_view s, std::string_view suf) {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

static bool is_scalar_load(std::string_view name) {
  return name == "lb" || name == "lh" || name == "lw" || name == "lbu" || name == "lhu" || name == "flw";
}

static bool is_scalar_store(std::string_view name) { return name == "sb" || name == "sh" || name == "sw" || name == "fsw"; }

static bool is_vector_load(std::string_view name) { return name == "vlw12_v" || name == "vlbu12_v" || name == "vlw_v"; }

static bool is_vector_store(std::string_view name) { return name == "vsw12_v" || name == "vsb12_v" || name == "vsw_v"; }

static bool is_any_branch(std::string_view name) {
  return name == "beq" || name == "bne" || name == "blt" || name == "bge" || name == "bltu" || name == "bgeu" ||
         name == "vbeq" || name == "vbne" || name == "vblt" || name == "vbge" || name == "vbltu" || name == "vbgeu";
}

static std::string format_operands(const sbt::DecodedInst &di) {
  std::string out;

  auto add = [&](const std::string &s) {
    if (s.empty()) return;
    if (!out.empty()) out += ", ";
    out += s;
  };

  // Branches: rs1, rs2, target
  if (di.imm_kind == sbt::ImmKind::B13 && is_any_branch(di.name)) {
    if (di.rs1_class != sbt::RegClass::None) add(fmt_reg(di.rs1_class, di.rs1));
    if (di.rs2_class != sbt::RegClass::None) add(fmt_reg(di.rs2_class, di.rs2));
    const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(di.pc) + static_cast<int64_t>(di.imm));
    add(hex_u32(target));
    return out;
  }

  // Jumps
  if (di.imm_kind == sbt::ImmKind::J21 && di.name == "jal") {
    if (di.rd_class != sbt::RegClass::None) add(fmt_reg(di.rd_class, di.rd));
    const uint32_t target = static_cast<uint32_t>(static_cast<int64_t>(di.pc) + static_cast<int64_t>(di.imm));
    add(hex_u32(target));
    return out;
  }

  if (di.imm_kind == sbt::ImmKind::I12 && di.name == "jalr") {
    if (di.rd_class != sbt::RegClass::None) add(fmt_reg(di.rd_class, di.rd));
    add(std::to_string(di.imm) + "(" + fmt_reg(di.rs1_class, di.rs1) + ")");
    return out;
  }

  // Scalar loads/stores: rd, imm(rs1) / rs2, imm(rs1)
  if (di.imm_kind == sbt::ImmKind::I12 && is_scalar_load(di.name)) {
    add(fmt_reg(di.rd_class, di.rd));
    add(std::to_string(di.imm) + "(" + fmt_reg(di.rs1_class, di.rs1) + ")");
    return out;
  }
  if (di.imm_kind == sbt::ImmKind::S12 && is_scalar_store(di.name)) {
    add(fmt_reg(di.rs2_class, di.rs2));
    add(std::to_string(di.imm) + "(" + fmt_reg(di.rs1_class, di.rs1) + ")");
    return out;
  }

  // Vector loads/stores: vd/vs2, imm(vs1)
  if (di.imm_kind == sbt::ImmKind::I12 && di.rd_class == sbt::RegClass::V && di.rs1_class == sbt::RegClass::V &&
      is_vector_load(di.name)) {
    add(fmt_reg(di.rd_class, di.rd));
    add(std::to_string(di.imm) + "(" + fmt_reg(di.rs1_class, di.rs1) + ")");
    return out;
  }
  if (di.imm_kind == sbt::ImmKind::S12 && di.rs2_class == sbt::RegClass::V && di.rs1_class == sbt::RegClass::V &&
      is_vector_store(di.name)) {
    add(fmt_reg(di.rs2_class, di.rs2));
    add(std::to_string(di.imm) + "(" + fmt_reg(di.rs1_class, di.rs1) + ")");
    return out;
  }

  // Some ops use a different operand order than "vd, vs2, vs1".
  // Spike/LLVM: vfmadd.vv vd, vs1, vs2
  if (di.name == "vfmadd_vv") {
    add(fmt_reg(di.rd_class, di.rd));
    add(fmt_reg(di.rs1_class, di.rs1));
    add(fmt_reg(di.rs2_class, di.rs2));
    return out;
  }

  // Vector ops in rvv-like order: vd, vs2, vs1/rs1/imm
  if (ends_with(di.name, "_vv") || ends_with(di.name, "_vx")) {
    add(fmt_reg(di.rd_class, di.rd));
    add(fmt_reg(di.rs2_class, di.rs2));
    add(fmt_reg(di.rs1_class, di.rs1));
    return out;
  }
  if (ends_with(di.name, "_vi")) {
    if ((di.name == "vadd12_vi" || di.name == "vsub12_vi") && di.rs1_class != sbt::RegClass::None) {
      // Custom I-type: vd, vs1, imm12
      add(fmt_reg(di.rd_class, di.rd));
      add(fmt_reg(di.rs1_class, di.rs1));
      add(std::to_string(di.imm));
      return out;
    }
    add(fmt_reg(di.rd_class, di.rd));
    add(fmt_reg(di.rs2_class, di.rs2));
    add(std::to_string(di.imm));
    return out;
  }
  if (ends_with(di.name, "_v") && di.rs2_class != sbt::RegClass::None) {
    add(fmt_reg(di.rd_class, di.rd));
    add(fmt_reg(di.rs2_class, di.rs2));
    return out;
  }

  // Generic fallback: rd, rs1, rs2, rs3, imm
  if (di.rd_class != sbt::RegClass::None) add(fmt_reg(di.rd_class, di.rd));
  if (di.rs1_class != sbt::RegClass::None) add(fmt_reg(di.rs1_class, di.rs1));
  if (di.rs2_class != sbt::RegClass::None) add(fmt_reg(di.rs2_class, di.rs2));
  if (di.rs3_class != sbt::RegClass::None) add(fmt_reg(di.rs3_class, di.rs3));
  if (di.imm_kind != sbt::ImmKind::None) add(std::to_string(di.imm));
  return out;
}

static fs::path default_dump_path(const fs::path &elf) {
  fs::path p = elf;
  p.replace_extension(".dump");
  return p;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return 2;
  }

  const std::string cmd = argv[1];
  const fs::path elf_path = argv[2];

  fs::path dump_path = default_dump_path(elf_path);
  std::optional<std::string> func;
  std::optional<fs::path> json_out;
  bool require_known = false;
  bool bundle_regext = true;
  bool include_start = false;
  bool verbose = false;

  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--dump" && i + 1 < argc) {
      dump_path = argv[++i];
    } else if (a == "--func" && i + 1 < argc) {
      func = argv[++i];
    } else if (a == "--json" && i + 1 < argc) {
      json_out = fs::path(argv[++i]);
    } else if (a == "--require-known") {
      require_known = true;
    } else if (a == "--no-bundle-regext") {
      bundle_regext = false;
    } else if (a == "--include-start") {
      include_start = true;
    } else if (a == "--verbose") {
      verbose = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "未知参数: " << a << "\n";
      usage();
      return 2;
    }
  }

  try {
    if (cmd == "funcs") {
      const auto syms = sbt::elf::read_func_symbols(elf_path);
      for (const auto &s : syms) {
        std::cout << hex_u32(s.addr) << " size=" << s.size << " " << s.name << "\n";
      }
      return 0;
    }

    if (cmd == "verify") {
      const auto text = sbt::elf::read_section(elf_path, ".text");
      const auto dump = sbt::dump::parse_dump(dump_path);

      size_t mism = 0;
      for (size_t off = 0; off + 4 <= text.data.size(); off += 4) {
        const uint32_t pc = text.vaddr + static_cast<uint32_t>(off);
        auto it = dump.find(pc);
        if (it == dump.end()) {
          ++mism;
          if (mism <= 10) std::cerr << "dump 缺失 pc=" << hex_u32(pc) << "\n";
          continue;
        }
        const auto &b = it->second.bytes;
        for (int i = 0; i < 4; ++i) {
          if (text.data[off + i] != b[i]) {
            ++mism;
            if (mism <= 10) {
              std::cerr << "byte mismatch pc=" << hex_u32(pc) << " i=" << i << "\n";
            }
            break;
          }
        }
      }
      if (mism != 0) {
        std::cerr << "verify failed mismatches=" << mism << "\n";
        return 1;
      }
      std::cout << "verify ok: " << elf_path.string() << "\n";
      return 0;
    }

    if (cmd == "decode") {
      const auto text = sbt::elf::read_section(elf_path, ".text");
      const auto syms = sbt::elf::read_func_symbols(elf_path);
      const auto patterns = build_patterns_from_subset_header();

      sbt::DecodeOptions opt;
      opt.bundle_regext = bundle_regext;
      opt.require_known = require_known;

      std::vector<uint8_t> slice = text.data;
      uint32_t base = text.vaddr;

      if (func) {
        const auto it = std::find_if(syms.begin(), syms.end(), [&](const sbt::elf::FuncSymbol &s) { return s.name == *func; });
        if (it == syms.end()) {
          std::cerr << "未找到函数符号: " << *func << "\n";
          return 1;
        }
        const uint32_t start = it->addr;
        uint32_t end = (it->size != 0 ? start + it->size : start);
        if (end <= start) {
          // Fallback: decode until end of .text
          end = text.vaddr + static_cast<uint32_t>(text.data.size());
        }
        if (start < text.vaddr || end > text.vaddr + text.data.size()) {
          std::cerr << "函数范围越界: " << *func << " start=" << hex_u32(start) << " end=" << hex_u32(end) << "\n";
          return 1;
        }
        const size_t off = start - text.vaddr;
        const size_t len = end - start;
        slice.assign(text.data.begin() + static_cast<long>(off), text.data.begin() + static_cast<long>(off + len));
        base = start;
      }

      const auto decoded = sbt::decode_text(slice, base, opt, patterns);

      std::ostream *os = &std::cout;
      std::ofstream fout;
      if (json_out) {
        fout.open(*json_out);
        if (!fout) {
          std::cerr << "无法写入: " << json_out->string() << "\n";
          return 1;
        }
        os = &fout;
      }

      *os << "{\n";
      *os << "  \"elf\": \"" << json_escape(elf_path.string()) << "\",\n";
      *os << "  \"text_vaddr\": \"" << hex_u32(text.vaddr) << "\",\n";
      *os << "  \"text_size\": " << text.data.size() << ",\n";
      if (func) *os << "  \"func\": \"" << json_escape(*func) << "\",\n";
      *os << "  \"decoded\": [\n";
      for (size_t i = 0; i < decoded.size(); ++i) {
        const auto &di = decoded[i];
        *os << "    {\"pc\":\"" << hex_u32(di.pc) << "\",\"word\":\"" << hex_u32(di.word) << "\",\"name\":\""
            << json_escape(di.name) << "\"";
        if (di.rd_class != sbt::RegClass::None) *os << ",\"rd_class\":\"" << sbt::to_string(di.rd_class) << "\",\"rd\":" << di.rd;
        if (di.rs1_class != sbt::RegClass::None) *os << ",\"rs1_class\":\"" << sbt::to_string(di.rs1_class) << "\",\"rs1\":" << di.rs1;
        if (di.rs2_class != sbt::RegClass::None) *os << ",\"rs2_class\":\"" << sbt::to_string(di.rs2_class) << "\",\"rs2\":" << di.rs2;
        if (di.imm_kind != sbt::ImmKind::None) *os << ",\"imm_kind\":\"" << sbt::to_string(di.imm_kind) << "\",\"imm\":" << di.imm;
        if (di.custom.valid) {
          *os << ",\"custom\":{\"family\":\"" << sbt::to_string(di.custom.family) << "\",\"subop\":\"" << sbt::to_string(di.custom.subop)
              << "\",\"dtype\":\"" << sbt::to_string(di.custom.dtype) << "\",\"vm_bit\":" << (di.custom.vm_bit ? "true" : "false")
              << ",\"funct6\":" << unsigned(di.custom.funct6) << ",\"funct3\":" << unsigned(di.custom.funct3) << "}";
        }
        if (di.had_regext) {
          *os << ",\"regext\":{\"pc\":\"" << hex_u32(di.regext.pc) << "\",\"imm12\":" << di.regext.imm12
              << ",\"ext_rd\":" << unsigned(di.regext.ext_rd) << ",\"ext_rs1\":" << unsigned(di.regext.ext_rs1)
              << ",\"ext_rs2\":" << unsigned(di.regext.ext_rs2) << ",\"ext_rs3\":" << unsigned(di.regext.ext_rs3) << "}";
        }
        *os << "}" << (i + 1 == decoded.size() ? "\n" : ",\n");
      }
      *os << "  ]\n";
      *os << "}\n";
      return 0;
    }

    if (cmd == "pretty") {
      const auto text = sbt::elf::read_section(elf_path, ".text");
      const auto syms = sbt::elf::read_func_symbols(elf_path);
      const auto patterns = build_patterns_from_subset_header();

      sbt::DecodeOptions opt;
      opt.bundle_regext = bundle_regext;
      opt.require_known = require_known;

      std::vector<uint8_t> slice = text.data;
      uint32_t base = text.vaddr;

      if (func) {
        const auto it = std::find_if(syms.begin(), syms.end(), [&](const sbt::elf::FuncSymbol &s) { return s.name == *func; });
        if (it == syms.end()) {
          std::cerr << "未找到函数符号: " << *func << "\n";
          return 1;
        }
        const uint32_t start = it->addr;
        uint32_t end = (it->size != 0 ? start + it->size : start);
        if (end <= start) {
          // Fallback: decode until end of .text
          end = text.vaddr + static_cast<uint32_t>(text.data.size());
        }
        if (start < text.vaddr || end > text.vaddr + text.data.size()) {
          std::cerr << "函数范围越界: " << *func << " start=" << hex_u32(start) << " end=" << hex_u32(end) << "\n";
          return 1;
        }
        const size_t off = start - text.vaddr;
        const size_t len = end - start;
        slice.assign(text.data.begin() + static_cast<long>(off), text.data.begin() + static_cast<long>(off + len));
        base = start;
      }

      const auto decoded = sbt::decode_text(slice, base, opt, patterns);
      for (const auto &di : decoded) {
        std::cout << hex8(di.pc) << ": " << bytes_u32_le(di.word) << "  " << pretty_mnemonic(di.name);
        const std::string ops = format_operands(di);
        if (!ops.empty()) std::cout << "\t" << ops;
        if (di.had_regext) {
          std::cout << " ; regext imm12=" << di.regext.imm12;
        }
        std::cout << "\n";
      }
      return 0;
    }

    if (cmd == "cfgverify") {
      const auto text = sbt::elf::read_section(elf_path, ".text");
      const auto syms = sbt::elf::read_func_symbols(elf_path);
      const auto patterns = build_patterns_from_subset_header();

      sbt::DecodeOptions opt;
      opt.bundle_regext = bundle_regext;
      opt.require_known = require_known;

      struct FuncRange final {
        std::string name;
        uint32_t start = 0;
        uint32_t end = 0; // exclusive
      };

      auto text_end = text.vaddr + static_cast<uint32_t>(text.data.size());

      auto compute_range = [&](size_t idx) -> std::optional<FuncRange> {
        if (idx >= syms.size()) return std::nullopt;
        const auto &s = syms[idx];
        if (s.addr < text.vaddr || s.addr >= text_end) return std::nullopt;
        uint32_t start = s.addr;
        uint32_t end = 0;
        if (s.size != 0) {
          end = start + s.size;
        } else {
          for (size_t j = idx + 1; j < syms.size(); ++j) {
            if (syms[j].addr > start) {
              end = syms[j].addr;
              break;
            }
          }
          if (end == 0) end = text_end;
        }
        if (end <= start) end = text_end;
        if (end > text_end) end = text_end;
        FuncRange fr;
        fr.name = s.name;
        fr.start = start;
        fr.end = end;
        return fr;
      };

      std::vector<FuncRange> funcs;
      if (func) {
        const auto it = std::find_if(syms.begin(), syms.end(), [&](const sbt::elf::FuncSymbol &s) { return s.name == *func; });
        if (it == syms.end()) {
          std::cerr << "未找到函数符号: " << *func << "\n";
          return 1;
        }
        const size_t idx = static_cast<size_t>(std::distance(syms.begin(), it));
        auto fr = compute_range(idx);
        if (!fr) {
          std::cerr << "函数不在 .text 范围内: " << *func << "\n";
          return 1;
        }
        funcs.push_back(*fr);
      } else {
        funcs.reserve(syms.size() / 2);
        for (size_t i = 0; i < syms.size(); ++i) {
          if (!include_start && syms[i].name == "_start") continue;
          auto fr = compute_range(i);
          if (!fr) continue;
          funcs.push_back(*fr);
        }
      }

      std::ostream *os = &std::cout;
      std::ofstream fout;
      if (json_out) {
        fout.open(*json_out);
        if (!fout) {
          std::cerr << "无法写入: " << json_out->string() << "\n";
          return 1;
        }
        os = &fout;
      }

      struct Totals final {
        size_t funcs = 0;
        size_t blocks = 0;
        size_t edges = 0;
        size_t vbranch = 0;
        size_t vbranch_ok = 0;
        size_t vbranch_fail = 0;
        size_t barrier = 0;
        size_t barrier_ok = 0;
        size_t barrier_fail = 0;
        size_t unsupported_jalr = 0;
      } totals;

      bool all_ok = true;
      std::vector<sbt::cfg::FunctionVerifyResult> results;
      results.reserve(funcs.size());

      for (const auto &fr : funcs) {
        const size_t off = fr.start - text.vaddr;
        const size_t len = fr.end - fr.start;
        if (off + len > text.data.size()) {
          if (verbose) {
            std::cerr << "跳过函数（越界）: " << fr.name << " start=" << hex_u32(fr.start) << " end=" << hex_u32(fr.end) << "\n";
          }
          continue;
        }
        std::vector<uint8_t> slice(text.data.begin() + static_cast<long>(off), text.data.begin() + static_cast<long>(off + len));

        const auto decoded = sbt::decode_text(slice, fr.start, opt, patterns);
        const auto cfg = sbt::cfg::build_function_cfg(decoded, fr.start, fr.end);
        auto res = sbt::cfg::verify_function(cfg, fr.name);

        const size_t vb_ok = std::count_if(res.vbranch.begin(), res.vbranch.end(), [](const sbt::cfg::VBranchCheck &c) { return c.error.empty(); });
        const size_t vb_fail = res.vbranch.size() - vb_ok;
        const size_t bar_ok = std::count_if(res.barriers.begin(), res.barriers.end(), [](const sbt::cfg::BarrierCheck &c) { return c.ok; });
        const size_t bar_fail = res.barriers.size() - bar_ok;

        totals.funcs += 1;
        totals.blocks += res.blocks;
        totals.edges += res.edges;
        totals.vbranch += res.vbranch.size();
        totals.vbranch_ok += vb_ok;
        totals.vbranch_fail += vb_fail;
        totals.barrier += res.barriers.size();
        totals.barrier_ok += bar_ok;
        totals.barrier_fail += bar_fail;
        totals.unsupported_jalr += res.unsupported_jalr.size();

        const bool ok = (vb_fail == 0) && (bar_fail == 0) && res.unsupported_jalr.empty();
        if (!ok) all_ok = false;

        if (verbose) {
          std::cerr << "- " << fr.name << " blocks=" << res.blocks << " edges=" << res.edges << " vbranch_ok=" << vb_ok << "/"
                    << res.vbranch.size() << " barrier_ok=" << bar_ok << "/" << res.barriers.size()
                    << " unsupported_jalr=" << res.unsupported_jalr.size() << "\n";
        }

        results.push_back(std::move(res));
      }

      auto json_opt_hex = [&](const std::optional<uint32_t> &v) -> std::string {
        if (!v) return "null";
        return "\"" + hex_u32(*v) + "\"";
      };

      *os << "{\n";
      *os << "  \"elf\": \"" << json_escape(elf_path.string()) << "\",\n";
      *os << "  \"ok\": " << (all_ok ? "true" : "false") << ",\n";
      *os << "  \"functions\": [\n";
      for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        *os << "    {\n";
        *os << "      \"name\": \"" << json_escape(r.func) << "\",\n";
        *os << "      \"start\": \"" << hex_u32(r.start) << "\",\n";
        *os << "      \"end\": \"" << hex_u32(r.end) << "\",\n";
        *os << "      \"insts\": " << r.insts << ",\n";
        *os << "      \"blocks\": " << r.blocks << ",\n";
        *os << "      \"edges\": " << r.edges << ",\n";

        *os << "      \"vbranch\": [\n";
        for (size_t j = 0; j < r.vbranch.size(); ++j) {
          const auto &c = r.vbranch[j];
          *os << "        {\"addr\":\"" << hex_u32(c.vbranch_addr) << "\",\"block\":\"" << hex_u32(c.vbranch_block) << "\",\"mnemonic\":\""
              << json_escape(c.mnemonic) << "\",\"target\":" << json_opt_hex(c.target) << ",\"fallthrough\":" << json_opt_hex(c.fallthrough)
              << ",\"join_pc\":" << json_opt_hex(c.join_pc) << ",\"proven_uniform\":" << (c.proven_uniform ? "true" : "false")
              << ",\"join_is_join_inst\":" << (c.join_is_join_inst ? "true" : "false")
              << ",\"loop_like\":" << (c.loop_like ? "true" : "false") << ",\"postdom_ok\":" << (c.postdom_ok ? "true" : "false")
              << ",\"no_side_exit_ok\":" << (c.no_side_exit_ok ? "true" : "false") << ",\"single_entry_ok\":"
              << (c.single_entry_ok ? "true" : "false") << ",\"error\":";
          if (c.error.empty()) {
            *os << "null";
          } else {
            *os << "\"" << json_escape(c.error) << "\"";
          }
          *os << "}" << (j + 1 == r.vbranch.size() ? "\n" : ",\n");
        }
        *os << "      ],\n";

        *os << "      \"barriers\": [\n";
        for (size_t j = 0; j < r.barriers.size(); ++j) {
          const auto &c = r.barriers[j];
          *os << "        {\"addr\":\"" << hex_u32(c.barrier_addr) << "\",\"block\":\"" << hex_u32(c.barrier_block) << "\",\"ok\":"
              << (c.ok ? "true" : "false") << ",\"error\":";
          if (c.ok || c.error.empty()) {
            *os << "null";
          } else {
            *os << "\"" << json_escape(c.error) << "\"";
          }
          *os << "}" << (j + 1 == r.barriers.size() ? "\n" : ",\n");
        }
        *os << "      ],\n";

        *os << "      \"unsupported_jalr\": [\n";
        for (size_t j = 0; j < r.unsupported_jalr.size(); ++j) {
          const auto &u = r.unsupported_jalr[j];
          *os << "        {\"addr\":\"" << hex_u32(u.addr) << "\",\"word\":\"" << hex_u32(u.word) << "\"}"
              << (j + 1 == r.unsupported_jalr.size() ? "\n" : ",\n");
        }
        *os << "      ]\n";

        *os << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
      }
      *os << "  ],\n";

      *os << "  \"stats\": {\n";
      *os << "    \"funcs\": " << totals.funcs << ",\n";
      *os << "    \"blocks\": " << totals.blocks << ",\n";
      *os << "    \"edges\": " << totals.edges << ",\n";
      *os << "    \"vbranch\": " << totals.vbranch << ",\n";
      *os << "    \"vbranch_ok\": " << totals.vbranch_ok << ",\n";
      *os << "    \"vbranch_fail\": " << totals.vbranch_fail << ",\n";
      *os << "    \"barrier\": " << totals.barrier << ",\n";
      *os << "    \"barrier_ok\": " << totals.barrier_ok << ",\n";
      *os << "    \"barrier_fail\": " << totals.barrier_fail << ",\n";
      *os << "    \"unsupported_jalr\": " << totals.unsupported_jalr << "\n";
      *os << "  }\n";
      *os << "}\n";

      return all_ok ? 0 : 2;
    }

    std::cerr << "未知命令: " << cmd << "\n";
    usage();
    return 2;
  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << "\n";
    return 1;
  }
}
