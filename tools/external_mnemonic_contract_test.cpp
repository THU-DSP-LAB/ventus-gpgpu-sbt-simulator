#include "sbt/ptx_emit.hpp"
#include "sbt/riscv_decode.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string &msg) {
  if (!ok) throw std::runtime_error("assert: " + msg);
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t w) {
  out.push_back(static_cast<uint8_t>(w & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
}

uint32_t make_i_word(uint32_t opcode, uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t imm12) {
  uint32_t w = 0;
  w |= (opcode & 0x7Fu);
  w |= (rd & 0x1Fu) << 7;
  w |= (funct3 & 0x7u) << 12;
  w |= (rs1 & 0x1Fu) << 15;
  w |= (imm12 & 0xFFFu) << 20;
  return w;
}

std::string pretty_mnemonic(std::string name) {
  for (char &c : name) {
    if (c == '_') c = '.';
  }
  return name;
}

std::string json_name_field(const sbt::DecodedInst &di) { return "\"name\":\"" + di.name + "\""; }

void check_pretty_and_json_names_survive_descriptor_migration() {
  const uint32_t word = make_i_word(/*opcode=*/0x13u, /*funct3=*/0x0u, /*rd=*/3u, /*rs1=*/11u, /*imm12=*/0x15u);
  std::vector<uint8_t> text;
  append_u32_le(text, word);

  sbt::DecodeOptions opt;
  opt.require_known = true;
  const std::vector<sbt::Pattern> patterns = {{
      sbt::Pattern{"vadd12_vi", word, 0xFFFF'FFFFu},
  }};
  const auto decoded = sbt::decode_text(text, /*text_vaddr=*/0x80000000u, opt, patterns);
  require(decoded.size() == 1, "single instruction decoded");
  require(decoded[0].name == "vadd12_vi", "external mnemonic contract should keep canonical name");
  require(pretty_mnemonic(decoded[0].name) == "vadd12.vi", "pretty mnemonic should still expose mnemonic-derived spelling");
  require(json_name_field(decoded[0]) == "\"name\":\"vadd12_vi\"", "json/reporting should still expose canonical mnemonic");
}

void check_coverage_style_reporting_still_uses_name() {
  const auto *meta = sbt::find_inst_metadata("beq");
  require(meta != nullptr, "beq metadata should stay reachable by name");
  require(std::string(meta->name) == "beq", "coverage/reporting should still observe canonical mnemonic");
}

void check_builtin_symbol_contract_is_unchanged() {
  require(sbt::ptx::is_inlined_builtin_call_name("_Z4sqrtf"), "sqrt builtin should remain inlined by external symbol name");
  require(sbt::ptx::is_inlined_builtin_call_name("__builtin_riscv_global_id_x"),
          "builtin reporting/inlining should remain keyed by external symbol name");
  require(!sbt::ptx::is_inlined_builtin_call_name("__not_a_builtin"), "unknown external symbols must not be misclassified");
}

} // namespace

int main() {
  check_pretty_and_json_names_survive_descriptor_migration();
  check_coverage_style_reporting_still_uses_name();
  check_builtin_symbol_contract_is_unchanged();
  std::cout << "ok external mnemonic contract\n";
  return 0;
}
