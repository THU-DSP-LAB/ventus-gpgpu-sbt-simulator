#include "sbt/builtin_semantics.hpp"

#include <array>
#include <string_view>

namespace sbt {
namespace {

static constexpr std::array<BuiltinEntry, 34> kBuiltinCalls{{
    {"_Z13get_global_idj", BuiltinKind::GetGlobalId},
    {"_Z12get_local_idj", BuiltinKind::GetLocalId},
    {"_Z12get_group_idj", BuiltinKind::GetGroupId},
    {"_Z15get_global_sizej", BuiltinKind::GetGlobalSize},
    {"__builtin_riscv_workitem_id_x", BuiltinKind::WorkitemIdX},
    {"__builtin_riscv_workitem_id_y", BuiltinKind::WorkitemIdY},
    {"__builtin_riscv_workitem_id_z", BuiltinKind::WorkitemIdZ},
    {"__builtin_riscv_workgroup_id_x", BuiltinKind::WorkgroupIdX},
    {"__builtin_riscv_workgroup_id_y", BuiltinKind::WorkgroupIdY},
    {"__builtin_riscv_workgroup_id_z", BuiltinKind::WorkgroupIdZ},
    {"__builtin_riscv_local_size_x", BuiltinKind::LocalSizeX},
    {"__builtin_riscv_local_size_y", BuiltinKind::LocalSizeY},
    {"__builtin_riscv_local_size_z", BuiltinKind::LocalSizeZ},
    {"__builtin_riscv_global_id_x", BuiltinKind::GlobalIdX},
    {"__builtin_riscv_global_id_y", BuiltinKind::GlobalIdY},
    {"__builtin_riscv_global_id_z", BuiltinKind::GlobalIdZ},
    {"_Z10__clc_sqrtf", BuiltinKind::SqrtF},
    {"_Z4sqrtf", BuiltinKind::SqrtF},
    {"_Z4fmaxff", BuiltinKind::FmaxF},
    {"_Z3cosDv4_f", BuiltinKind::Vec4Cos},
    {"_Z3sinDv4_f", BuiltinKind::Vec4Sin},
    {"_Z3tanDv4_f", BuiltinKind::Vec4Tan},
    {"_Z4sqrtDv4_f", BuiltinKind::Vec4Sqrt},
    {"_Z4fabsDv4_f", BuiltinKind::Vec4Fabs},
    {"_Z5mad24iii", BuiltinKind::Mad24},
    {"_Z20work_group_broadcastfj", BuiltinKind::WorkGroupBroadcast1D32},
    {"_Z20work_group_broadcastfjj", BuiltinKind::WorkGroupBroadcast2D32},
    {"_Z20work_group_broadcastfjjj", BuiltinKind::WorkGroupBroadcast3D32},
    {"_Z20work_group_broadcastij", BuiltinKind::WorkGroupBroadcast1D32},
    {"_Z20work_group_broadcastijj", BuiltinKind::WorkGroupBroadcast2D32},
    {"_Z20work_group_broadcastijjj", BuiltinKind::WorkGroupBroadcast3D32},
    {"_Z20work_group_broadcastjj", BuiltinKind::WorkGroupBroadcast1D32},
    {"_Z20work_group_broadcastjjj", BuiltinKind::WorkGroupBroadcast2D32},
    {"_Z20work_group_broadcastjjjj", BuiltinKind::WorkGroupBroadcast3D32},
}};

static constexpr std::array<int, 0> kNoInputs{};
static constexpr std::array<int, 1> kInputV0{{0}};
static constexpr std::array<int, 1> kInputV1{{1}};
static constexpr std::array<int, 1> kInputV2{{2}};
static constexpr std::array<int, 1> kInputV3{{3}};
static constexpr std::array<int, 2> kInputV0V1{{0, 1}};
static constexpr std::array<int, 3> kInputV0V1V2{{0, 1, 2}};

static constexpr std::array<VectorWriteSummary, 1> kV0WorkItemVarying{{
    {0, BuiltinUniformTransfer::WorkItemVarying, kNoInputs},
}};
static constexpr std::array<VectorWriteSummary, 1> kV0WorkGroupUniform{{
    {0, BuiltinUniformTransfer::WorkGroupUniform, kNoInputs},
}};
static constexpr std::array<VectorWriteSummary, 1> kV0SameAsInputV0{{
    {0, BuiltinUniformTransfer::SameAsInputs, kInputV0},
}};
static constexpr std::array<VectorWriteSummary, 1> kV0SameAsInputV0V1{{
    {0, BuiltinUniformTransfer::SameAsInputs, kInputV0V1},
}};
static constexpr std::array<VectorWriteSummary, 1> kV0SameAsInputV0V1V2{{
    {0, BuiltinUniformTransfer::SameAsInputs, kInputV0V1V2},
}};
static constexpr std::array<VectorWriteSummary, 4> kVec4SameAsCorrespondingInput{{
    {0, BuiltinUniformTransfer::SameAsInputs, kInputV0},
    {1, BuiltinUniformTransfer::SameAsInputs, kInputV1},
    {2, BuiltinUniformTransfer::SameAsInputs, kInputV2},
    {3, BuiltinUniformTransfer::SameAsInputs, kInputV3},
}};

static constexpr std::array<BuiltinSummary, 27> kBuiltinSummaries{{
    {BuiltinKind::GetGlobalId, kV0WorkItemVarying},
    {BuiltinKind::GetLocalId, kV0WorkItemVarying},
    {BuiltinKind::GetGroupId, kV0SameAsInputV0},
    {BuiltinKind::GetGlobalSize, kV0SameAsInputV0},
    {BuiltinKind::WorkitemIdX, kV0WorkItemVarying},
    {BuiltinKind::WorkitemIdY, kV0WorkItemVarying},
    {BuiltinKind::WorkitemIdZ, kV0WorkItemVarying},
    {BuiltinKind::WorkgroupIdX, kV0WorkGroupUniform},
    {BuiltinKind::WorkgroupIdY, kV0WorkGroupUniform},
    {BuiltinKind::WorkgroupIdZ, kV0WorkGroupUniform},
    {BuiltinKind::LocalSizeX, kV0WorkGroupUniform},
    {BuiltinKind::LocalSizeY, kV0WorkGroupUniform},
    {BuiltinKind::LocalSizeZ, kV0WorkGroupUniform},
    {BuiltinKind::GlobalIdX, kV0WorkItemVarying},
    {BuiltinKind::GlobalIdY, kV0WorkItemVarying},
    {BuiltinKind::GlobalIdZ, kV0WorkItemVarying},
    {BuiltinKind::SqrtF, kV0SameAsInputV0},
    {BuiltinKind::FmaxF, kV0SameAsInputV0V1},
    {BuiltinKind::Vec4Cos, kVec4SameAsCorrespondingInput},
    {BuiltinKind::Vec4Sin, kVec4SameAsCorrespondingInput},
    {BuiltinKind::Vec4Tan, kVec4SameAsCorrespondingInput},
    {BuiltinKind::Vec4Sqrt, kVec4SameAsCorrespondingInput},
    {BuiltinKind::Vec4Fabs, kVec4SameAsCorrespondingInput},
    {BuiltinKind::Mad24, kV0SameAsInputV0V1V2},
    {BuiltinKind::WorkGroupBroadcast1D32, kV0WorkGroupUniform, true},
    {BuiltinKind::WorkGroupBroadcast2D32, kV0WorkGroupUniform, true},
    {BuiltinKind::WorkGroupBroadcast3D32, kV0WorkGroupUniform, true},
}};

} // namespace

std::span<const BuiltinEntry> builtin_call_entries() { return kBuiltinCalls; }

std::span<const BuiltinSummary> builtin_summaries() {
  return kBuiltinSummaries;
}

std::optional<BuiltinKind> lookup_builtin_call(std::string_view callee) {
  for (const auto &entry : kBuiltinCalls) {
    if (entry.name == callee)
      return entry.kind;
  }
  return std::nullopt;
}

bool is_inlined_builtin_call_name(std::string_view callee) {
  return lookup_builtin_call(callee).has_value();
}

bool is_work_group_broadcast_call_name(std::string_view callee) {
  return callee.starts_with("_Z20work_group_broadcast");
}

std::optional<BuiltinSummary> builtin_summary_for(BuiltinKind kind) {
  for (const auto &summary : kBuiltinSummaries) {
    if (summary.kind == kind)
      return summary;
  }
  return std::nullopt;
}

} // namespace sbt
