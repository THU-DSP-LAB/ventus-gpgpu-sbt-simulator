#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sbt {

enum class RegClass : uint8_t {
  None = 0,
  X = 1,
  V = 2,
};

enum class ImmKind : uint8_t {
  None = 0,
  I12,
  S12,
  B13,
  U20,
  J21,
  CSR12,
  UImm5,
  SImm5,
  Raw12,
};

struct RegextPrefix final {
  bool valid = false;
  bool validi = false; // regexti: extends rs2/rd and immediate
  uint32_t pc = 0;
  uint16_t imm12 = 0;
  uint8_t ext_rd = 0;
  uint8_t ext_rs1 = 0;
  uint8_t ext_rs2 = 0;
  uint8_t ext_rs3 = 0;
  uint8_t ext_imm = 0; // 6-bit field (meaningful when validi=true)
};

struct DecodedInst final {
  uint32_t pc = 0;
  uint32_t word = 0;
  std::string name;

  RegClass rd_class = RegClass::None;
  RegClass rs1_class = RegClass::None;
  RegClass rs2_class = RegClass::None;
  RegClass rs3_class = RegClass::None;

  int rd = -1;
  int rs1 = -1;
  int rs2 = -1;
  int rs3 = -1;

  ImmKind imm_kind = ImmKind::None;
  int32_t imm = 0;

  bool had_regext = false;
  RegextPrefix regext{};
};

struct DecodeOptions final {
  bool bundle_regext = true;
  bool require_known = false;
};

struct Pattern final {
  const char *name = nullptr;
  uint32_t match = 0;
  uint32_t mask = 0;
};

std::vector<DecodedInst>
decode_text(const std::vector<uint8_t> &text, uint32_t text_vaddr, const DecodeOptions &opt,
            const std::vector<Pattern> &patterns);

const char *to_string(RegClass c);
const char *to_string(ImmKind k);

} // namespace sbt
