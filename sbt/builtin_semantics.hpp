#pragma once

#include <optional>
#include <span>
#include <string_view>

namespace sbt {

enum class BuiltinKind {
  GetGlobalId,
  GetLocalId,
  GetGroupId,
  GetGlobalSize,
  WorkitemIdX,
  WorkitemIdY,
  WorkitemIdZ,
  WorkgroupIdX,
  WorkgroupIdY,
  WorkgroupIdZ,
  LocalSizeX,
  LocalSizeY,
  LocalSizeZ,
  GlobalIdX,
  GlobalIdY,
  GlobalIdZ,
  SqrtF,
  FmaxF,
  Vec4Cos,
  Vec4Sin,
  Vec4Tan,
  Vec4Sqrt,
  Vec4Fabs,
  Mad24,
};

struct BuiltinEntry final {
  std::string_view name;
  BuiltinKind kind;
};

enum class BuiltinUniformTransfer {
  WorkGroupUniform,
  WorkItemVarying,
  SameAsInputs,
};

struct VectorWriteSummary final {
  int dst_vreg = -1;
  BuiltinUniformTransfer transfer = BuiltinUniformTransfer::WorkItemVarying;
  std::span<const int> input_vregs;
};

struct BuiltinSummary final {
  BuiltinKind kind;
  std::span<const VectorWriteSummary> vector_writes;
};

std::span<const BuiltinEntry> builtin_call_entries();
std::span<const BuiltinSummary> builtin_summaries();
std::optional<BuiltinKind> lookup_builtin_call(std::string_view callee);
bool is_inlined_builtin_call_name(std::string_view callee);
std::optional<BuiltinSummary> builtin_summary_for(BuiltinKind kind);

} // namespace sbt
