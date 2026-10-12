#include "sbt/ptx_cleanup.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace sbt::ptx::detail {
namespace {

struct Line final {
  std::string original;
  std::string predicate;
  std::string opcode;
  std::vector<std::string> operands;
  bool pure = false;
  bool boundary = true;
  bool removed = false;
};

using RegisterSet = std::unordered_set<std::string>;
using Aliases = std::unordered_map<std::string, std::string>;

struct Extent final {
  size_t first;
  size_t last;
};
using Extents = std::unordered_map<std::string, Extent>;

class Cleanup final {
public:
  explicit Cleanup(std::string_view body);
  std::string run();

private:
  std::vector<Line> lines;
  Aliases aliases;
  bool propagate_copies();
  bool propagate_line(size_t index);
  bool substitute_sources(size_t index);
  bool record_copy(size_t index);
  bool fuse_copy(size_t index, size_t start, const RegisterSet &live_after);
  Extents register_extents() const;
  RegisterSet live_after_segment(const Extents &extents, size_t start, size_t end) const;
  bool delete_dead_segment(const RegisterSet &live_out, size_t start, size_t end);
  bool delete_dead_scratch();
  bool delete_next_label_branches();
  bool next_label_is(size_t index, const std::string &target) const;
};

std::string trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

bool register_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.'; }

std::vector<std::string> split_operands(std::string_view text) {
  std::vector<std::string> result;
  size_t start = 0;
  int nesting = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '{' || ch == '[' || ch == '(') ++nesting;
    if (ch == '}' || ch == ']' || ch == ')') --nesting;
    if (ch != ',' || nesting != 0) continue;
    result.push_back(trim(text.substr(start, i - start)));
    start = i + 1;
  }
  result.push_back(trim(text.substr(start)));
  return result;
}

Line parse_line(const std::string &original) {
  Line line;
  line.original = original;
  auto text = trim(original);
  if (text.starts_with("//") || text.empty()) {
    line.boundary = false;
    return line;
  }
  if (!text.ends_with(';') || text.starts_with('.')) return line;
  if (text.starts_with('@')) {
    const auto end = text.find(' ');
    line.predicate = text.substr(0, end);
    text = trim(text.substr(end + 1));
  }
  const auto end = text.find(' ');
  line.opcode = text.substr(0, end);
  if (end != std::string::npos)
    line.operands = split_operands(std::string_view(text).substr(end + 1, text.size() - end - 2));
  const auto base = line.opcode.substr(0, line.opcode.find('.'));
  static const RegisterSet pure_ops{"mov",   "add",  "sub", "mul",   "mad", "fma", "div", "rem",  "min",  "max", "abs",
                                    "neg",   "and",  "or",  "xor",   "not", "shl", "shr", "setp", "selp", "cvt", "cvta",
                                    "bfind", "sqrt", "rcp", "rsqrt", "ex2", "lg2", "sin", "cos",  "tanh"};
  line.pure = pure_ops.contains(base) && line.opcode.find(".cc") == std::string::npos;
  // Collectives and control operations delimit value propagation even if they
  // have register outputs. Loads/stores stay observable and are never deleted.
  line.boundary = !line.pure && base != "ld" && base != "st";
  return line;
}

bool scratch_register(std::string_view reg) {
  static const RegisterSet legacy_scratch{"%r14", "%r15", "%r16", "%r17", "%r18", "%r19",
                                          "%r20", "%r21", "%r22", "%r23", "%r24", "%r25"};
  return reg.starts_with("%tmp_") || reg.starts_with("%f") || reg.starts_with("%h") || reg.starts_with("%ub") ||
         reg.starts_with("%uh") || legacy_scratch.contains(std::string(reg));
}

std::string bit_type(std::string_view reg) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> types{{{"%tmp_b64_", "b64"},
                                                                                        {"%rd", "b64"},
                                                                                        {"%tmp_b16_", "b16"},
                                                                                        {"%h", "b16"},
                                                                                        {"%tmp_p_", "pred"},
                                                                                        {"%p", "pred"},
                                                                                        {"%tmp_b32_", "b32"},
                                                                                        {"%x", "b32"},
                                                                                        {"%v", "b32"},
                                                                                        {"%r", "b32"}}};
  for (const auto &[prefix, type] : types) {
    if (!reg.starts_with(prefix)) continue;
    const auto suffix = reg.substr(prefix.size());
    if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
      return std::string(type);
  }
  return {};
}

std::string register_type(std::string_view reg) {
  if (reg.starts_with("%tmp_f32_") || reg.starts_with("%f")) return "b32";
  return bit_type(reg);
}

bool copy_instruction(const Line &line) {
  static const RegisterSet copies{"mov.b16", "mov.b32", "mov.b64", "mov.u32",
                                  "mov.s32", "mov.u64", "mov.s64", "mov.pred"};
  if (!copies.contains(line.opcode) || line.operands.size() != 2) return false;
  auto type = line.opcode.substr(line.opcode.find('.') + 1);
  if (type.front() == 'u' || type.front() == 's') type.front() = 'b';
  return register_type(line.operands.front()) == type;
}

RegisterSet writes(const Line &line) {
  if (line.boundary || line.operands.empty() || line.opcode.starts_with("st.")) return {};
  return ptx_registers(line.operands.front());
}

RegisterSet reads(const Line &line) {
  auto result = ptx_registers(line.predicate);
  const size_t start = writes(line).empty() ? 0 : 1;
  for (size_t i = start; i < line.operands.size(); ++i) {
    const auto regs = ptx_registers(line.operands[i]);
    result.insert(regs.begin(), regs.end());
  }
  return result;
}

std::string render(const Line &line) {
  if (line.opcode.empty() || line.boundary) return line.original;
  std::string result = "  " + (line.predicate.empty() ? "" : line.predicate + " ") + line.opcode + " ";
  for (size_t i = 0; i < line.operands.size(); ++i) {
    if (i != 0) result += ", ";
    result += line.operands[i];
  }
  return result + ";";
}

std::optional<Line> simplify_identity(const Line &line) {
  if (line.operands.size() != 3) return std::nullopt;
  const auto &op = line.opcode;
  const auto &a = line.operands[1];
  static const RegisterSet zero{"0", "0x00000000"};
  static const RegisterSet one{"1"};
  static const RegisterSet full_word{"0xffffffff", "-1"};
  static const std::unordered_map<std::string, RegisterSet> identities{
      {"add.u32", zero},   {"add.s32", zero},   {"add.u64", zero},     {"sub.u32", zero}, {"sub.s32", zero},
      {"sub.u64", zero},   {"or.b32", zero},    {"xor.b32", zero},     {"shl.b32", zero}, {"shr.u32", zero},
      {"shr.s32", zero},   {"shl.b64", zero},   {"shr.u64", zero},     {"shr.s64", zero}, {"mul.lo.u32", one},
      {"mul.lo.s32", one}, {"mul.lo.u64", one}, {"and.b32", full_word}};
  const auto identity = identities.find(op);
  if (identity == identities.end() || !identity->second.contains(line.operands[2])) return std::nullopt;
  auto result = line;
  result.opcode = op.ends_with("64") ? "mov.b64" : "mov.b32";
  result.operands = {line.operands.front(), a};
  return result;
}

std::string replace_registers(std::string_view operand, const std::unordered_map<std::string, std::string> &aliases) {
  std::string result;
  size_t cursor = 0;
  while (cursor < operand.size()) {
    if (operand[cursor] != '%') {
      result += operand[cursor++];
      continue;
    }
    size_t end = cursor + 1;
    while (end < operand.size() && register_char(operand[end]))
      ++end;
    const std::string reg(operand.substr(cursor, end - cursor));
    const auto alias = aliases.find(reg);
    result += alias == aliases.end() ? reg : alias->second;
    cursor = end;
  }
  return result;
}

bool Cleanup::propagate_copies() {
  aliases.clear();
  bool changed = false;
  for (size_t i = 0; i < lines.size(); ++i)
    changed |= propagate_line(i);
  return changed;
}

bool Cleanup::propagate_line(size_t index) {
  auto &line = lines[index];
  if (line.removed) return false;
  if (line.boundary) {
    aliases.clear();
    return false;
  }
  if (line.opcode.empty()) return false;
  bool changed = false;
  if (const auto identity = simplify_identity(line)) {
    line = *identity;
    changed = true;
  }
  changed |= substitute_sources(index);
  changed |= record_copy(index);
  return changed;
}

bool Cleanup::substitute_sources(size_t index) {
  auto &line = lines[index];
  bool changed = false;
  const size_t start = writes(line).empty() ? 0 : 1;
  for (size_t i = start; i < line.operands.size(); ++i) {
    const auto replacement = replace_registers(line.operands[i], aliases);
    changed |= replacement != line.operands[i];
    line.operands[i] = replacement;
  }
  const auto destinations = writes(line);
  std::erase_if(aliases, [&](const auto &alias) {
    return destinations.contains(alias.first) || destinations.contains(alias.second);
  });
  return changed;
}

bool Cleanup::record_copy(size_t index) {
  auto &line = lines[index];
  if (!copy_instruction(line)) return false;
  const auto &dst = line.operands[0];
  const auto &src = line.operands[1];
  if (dst == src) {
    line.removed = true;
    return true;
  }
  // Bit-size sources remain legal in every same-width operand position;
  // forwarding an f32 register into an integer operation would not be legal.
  if (line.predicate.empty() && !bit_type(src).empty() && bit_type(src) == register_type(dst)) aliases[dst] = src;
  return false;
}

bool compatible_writeback(const Line &producer, const Line &copy) {
  if (!producer.pure || producer.operands.empty()) return false;
  const auto &dst = copy.operands[0];
  const auto &src = copy.operands[1];
  if (producer.operands[0] != src || !scratch_register(src)) return false;
  const auto type = bit_type(dst);
  return !type.empty() && type != "pred" && type == register_type(src) && producer.predicate == copy.predicate;
}

bool ignorable(const Line &line) { return line.removed || (line.opcode.empty() && !line.boundary); }

bool Cleanup::fuse_copy(size_t index, size_t start, const RegisterSet &live_after) {
  auto &copy = lines[index];
  if (!copy_instruction(copy) || live_after.contains(copy.operands[1])) return false;
  size_t previous = index;
  while (previous > start) {
    auto &producer = lines[--previous];
    if (ignorable(producer)) continue;
    if (!compatible_writeback(producer, copy)) return false;
    producer.operands[0] = copy.operands[0];
    copy.removed = true;
    return true;
  }
  return false;
}

Extents Cleanup::register_extents() const {
  Extents extents;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].removed) continue;
    for (const auto &reg : ptx_registers(render(lines[i]))) {
      const auto [it, inserted] = extents.emplace(reg, Extent{i, i});
      if (!inserted) it->second.last = i;
    }
  }
  return extents;
}

bool guard_overwritten(std::string_view guard, const RegisterSet &destinations) {
  const auto regs = ptx_registers(guard);
  return std::any_of(regs.begin(), regs.end(), [&](const auto &reg) { return destinations.contains(reg); });
}

bool dead_scratch_result(const Line &line, const RegisterSet &live) {
  if (!line.pure) return false;
  const auto destinations = writes(line);
  return !destinations.empty() && std::all_of(destinations.begin(), destinations.end(), [&](const auto &reg) {
    return scratch_register(reg) && !live.contains(reg);
  });
}

RegisterSet Cleanup::live_after_segment(const Extents &extents, size_t start, size_t end) const {
  RegisterSet live;
  std::unordered_map<std::string, RegisterSet> defined_by_guard;
  for (size_t i = start; i < end; ++i) {
    if (lines[i].removed) continue;
    for (const auto &reg : ptx_registers(render(lines[i]))) {
      const auto extent = extents.at(reg);
      if (!scratch_register(reg) || extent.first < start || extent.last >= end) live.insert(reg);
    }
    // A read before an unconditional definition can carry a value around a
    // back-edge, even when every textual mention is within this one segment.
    for (const auto &reg : reads(lines[i])) {
      if (defined_by_guard[""].contains(reg)) continue;
      if (!defined_by_guard[lines[i].predicate].contains(reg)) live.insert(reg);
    }
    const auto destinations = writes(lines[i]);
    std::erase_if(defined_by_guard,
                  [&](const auto &definition) { return guard_overwritten(definition.first, destinations); });
    auto &defined = defined_by_guard[lines[i].predicate];
    defined.insert(destinations.begin(), destinations.end());
  }
  return live;
}

bool Cleanup::delete_dead_segment(const RegisterSet &live_out, size_t start, size_t end) {
  auto live = live_out;
  bool changed = false;
  for (size_t i = end; i > start;) {
    auto &line = lines[--i];
    if (line.removed || line.opcode.empty()) continue;
    if (fuse_copy(i, start, live)) {
      changed = true;
      continue;
    }
    if (dead_scratch_result(line, live)) {
      line.removed = true;
      changed = true;
      continue;
    }
    const auto destinations = writes(line);
    if (line.predicate.empty())
      for (const auto &reg : destinations)
        live.erase(reg);
    const auto sources = reads(line);
    live.insert(sources.begin(), sources.end());
  }
  return changed;
}

bool Cleanup::delete_dead_scratch() {
  const auto extents = register_extents();
  bool changed = false;
  size_t start = 0;
  while (start < lines.size()) {
    size_t end = start;
    while (end < lines.size() && (!lines[end].boundary || lines[end].removed))
      ++end;
    changed |= delete_dead_segment(live_after_segment(extents, start, end), start, end);
    start = end + 1;
  }
  return changed;
}

bool Cleanup::delete_next_label_branches() {
  bool changed = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    auto &branch = lines[i];
    if (branch.removed || !branch.predicate.empty() || branch.operands.size() != 1) continue;
    if (branch.opcode != "bra" && branch.opcode != "bra.uni") continue;
    if (!next_label_is(i, branch.operands[0])) continue;
    branch.removed = true;
    changed = true;
  }
  return changed;
}

bool Cleanup::next_label_is(size_t index, const std::string &target) const {
  for (size_t next = index + 1; next < lines.size(); ++next) {
    const auto &line = lines[next];
    if (line.removed) continue;
    const auto text = trim(line.original);
    if (text.empty() || text.starts_with("//")) continue;
    if (!text.ends_with(':')) return false;
    if (text != target + ":") continue;
    return true;
  }
  return false;
}

Cleanup::Cleanup(std::string_view body) {
  std::istringstream input{std::string(body)};
  for (std::string line; std::getline(input, line);)
    lines.push_back(parse_line(line));
}

std::string Cleanup::run() {
  bool changed;
  do {
    changed = propagate_copies();
    changed |= delete_dead_scratch();
    changed |= delete_next_label_branches();
  } while (changed);
  std::string result;
  for (const auto &line : lines)
    if (!line.removed) result += render(line) + "\n";
  return result;
}

} // namespace

std::unordered_set<std::string> ptx_registers(std::string_view text) {
  RegisterSet result;
  for (size_t cursor = 0; cursor < text.size(); ++cursor) {
    if (text[cursor] != '%') continue;
    const size_t start = cursor++;
    while (cursor < text.size() && register_char(text[cursor]))
      ++cursor;
    result.emplace(text.substr(start, cursor - start));
    --cursor;
  }
  return result;
}

std::string cleanup_ptx_body(std::string_view body) {
  Cleanup cleanup(body);
  return cleanup.run();
}

} // namespace sbt::ptx::detail
