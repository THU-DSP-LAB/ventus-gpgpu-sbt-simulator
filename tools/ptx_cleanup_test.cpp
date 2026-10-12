/*
背景：SBT lowering 的同宽 bitcast 和整数 scratch 拷贝会产生多余 PTX。
需求/作用：验证清理的实际简化效果以及重定义、谓词、类型、控制流和副作用边界。
用法：cmake --build build && timeout 180 build/ptx_cleanup_test；统一 regress.sh
也运行本测试。 实现原理/处理步骤：表驱动检查清理结果及幂等性；真实 emitter /
ptxas / GPU 验证由其余 gate 覆盖。
*/
#include "sbt/ptx_cleanup.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Case final {
  std::string description;
  std::string input;
  std::vector<std::string> present;
  std::vector<std::string> absent;
};

void check_case(const Case &test) {
  const auto result = sbt::ptx::detail::cleanup_ptx_body(test.input);
  for (const auto &text : test.present) {
    if (result.find(text) == std::string::npos)
      throw std::runtime_error(test.description + ": missing " + text + "\n" + result);
  }
  for (const auto &text : test.absent) {
    if (result.find(text) != std::string::npos)
      throw std::runtime_error(test.description + ": redundant " + text + "\n" + result);
  }
  if (sbt::ptx::detail::cleanup_ptx_body(result) != result)
    throw std::runtime_error(test.description + ": cleanup is not idempotent");
}

std::vector<Case> copy_cases() {
  return {
      {"in-place SFU bitcasts",
       "mov.b32 %tmp_f32_2, %v6;\nex2.approx.f32 %tmp_f32_3, "
       "%tmp_f32_2;\nmov.b32 %v6, %tmp_f32_3;\n",
       {"ex2.approx.f32 %v6, %v6;"},
       {"mov.", "%tmp_"}},
      {"scalar copy chain",
       "mov.u32 %r14, %x5;\nmov.b32 %tmp_f32_0, %r14;\nsqrt.rn.f32 %tmp_f32_1, "
       "%tmp_f32_0;\n"
       "mov.b32 %tmp_b32_0, %tmp_f32_1;\nmov.u32 %x6, %tmp_b32_0;\n",
       {"sqrt.rn.f32 %x6, %x5;"},
       {"mov.", "%tmp_", "%r14"}},
      {"two FP inputs and overlapping destination",
       "mov.b32 %f0, %v1;\nmov.b32 %f1, %v2;\nfma.rn.f32 %f2, %f0, %f1, "
       "%v1;\nmov.b32 %v1, %f2;\n",
       {"fma.rn.f32 %v1, %v1, %v2, %v1;"},
       {"mov.", "%f"}},
      {"source redefinition preserves snapshot",
       "mov.u32 %tmp_b32_0, %v1;\nmov.u32 %v1, %v2;\nadd.u32 %v3, %tmp_b32_0, "
       "%v1;\n",
       {"mov.u32 %tmp_b32_0, %v1;", "add.u32 %v3, %tmp_b32_0, %v2;"},
       {}},
      {"predicated source redefinition invalidates aliases",
       "mov.u32 %tmp_b32_0, %v1;\n@%p1 mov.u32 %v1, %v2;\nadd.u32 %v3, "
       "%tmp_b32_0, %v1;\n",
       {"mov.u32 %tmp_b32_0, %v1;", "add.u32 %v3, %tmp_b32_0, %v1;"},
       {}},
      {"predicated copy cannot define unconditional alias",
       "@%p1 mov.b32 %tmp_f32_0, %v1;\nsqrt.rn.f32 %v2, %tmp_f32_0;\n",
       {"@%p1 mov.b32 %tmp_f32_0, %v1;", "sqrt.rn.f32 %v2, %tmp_f32_0;"},
       {}},
      {"different writeback predicates",
       "@%p1 sqrt.rn.f32 %tmp_f32_0, %v1;\n@%p2 mov.b32 %v2, %tmp_f32_0;\n",
       {"@%p1 sqrt.rn.f32 %tmp_f32_0, %v1;", "@%p2 mov.b32 %v2, %tmp_f32_0;"},
       {}},
      {"same writeback predicate",
       "@%p1 sqrt.rn.f32 %tmp_f32_0, %v1;\n@%p1 mov.b32 %v2, %tmp_f32_0;\n",
       {"@%p1 sqrt.rn.f32 %v2, %v1;"},
       {"mov.", "%tmp_"}},
      {"old destination needed by producer",
       "add.u32 %tmp_b32_0, %v1, %v2;\nselp.b32 %tmp_b32_0, %v3, %tmp_b32_0, "
       "%p1;\nmov.b32 %v4, %tmp_b32_0;\n",
       {"add.u32 %tmp_b32_0, %v1, %v2;", "selp.b32 %v4, %v3, %tmp_b32_0, %p1;"},
       {"mov."}},
      {"writeback source used later",
       "sqrt.rn.f32 %tmp_f32_0, %v1;\nmov.b32 %v2, %tmp_f32_0;\nadd.f32 %v3, "
       "%tmp_f32_0, %v4;\n",
       {"sqrt.rn.f32 %tmp_f32_0, %v1;", "mov.b32 %v2, %tmp_f32_0;"},
       {}},
  };
}

std::vector<Case> safety_cases() {
  return {
      {"loop-carried scratch cannot be deleted or coalesced",
       "L:\nadd.u32 %v1, %tmp_b32_0, 1;\nadd.u32 %tmp_b32_0, %v2, 1;\nmov.b32 %v3, %tmp_b32_0;\nbra L;\n",
       {"add.u32 %v1, %tmp_b32_0, 1;", "add.u32 %tmp_b32_0, %v2, 1;", "mov.b32 %v3, %tmp_b32_0;"},
       {}},
      {"loop-carried copy snapshot",
       "L:\nadd.u32 %v1, %tmp_b32_0, 1;\nmov.b32 %tmp_b32_0, %v2;\nbra L;\n",
       {"add.u32 %v1, %tmp_b32_0, 1;", "mov.b32 %tmp_b32_0, %v2;"},
       {}},
      {"predicate redefinition exposes previous loop value",
       "L:\n@%p1 mov.b32 %tmp_b32_0, %v1;\nnot.pred %p1, %p1;\n@%p1 mov.b32 %v2, %tmp_b32_0;\nbra L;\n",
       {"@%p1 mov.b32 %tmp_b32_0, %v1;", "not.pred %p1, %p1;", "@%p1 mov.b32 %v2, %tmp_b32_0;"},
       {}},
      {"temporary survives labels",
       "mov.b32 %tmp_b32_0, %v1;\nL:\nadd.u32 %v2, %tmp_b32_0, %v3;\n",
       {"mov.b32 %tmp_b32_0, %v1;", "add.u32 %v2, %tmp_b32_0, %v3;"},
       {}},
      {"temporary used in opaque collective",
       "mov.b32 %tmp_b32_0, %v1;\nshfl.sync.idx.b32 %v2, %tmp_b32_0, 0, 31, "
       "-1;\n",
       {"mov.b32 %tmp_b32_0, %v1;", "shfl.sync.idx.b32 %v2, %tmp_b32_0"},
       {}},
      {"call is propagation boundary",
       "mov.b32 %tmp_b32_0, %v1;\ncall.uni f;\nadd.u32 %v2, %tmp_b32_0, %v1;\n",
       {"call.uni f;", "add.u32 %v2, %tmp_b32_0, %v1;"},
       {}},
      {"dead scratch calculation chain",
       "add.u32 %tmp_b32_0, %v1, 7;\nmul.lo.u32 %tmp_b32_1, %tmp_b32_0, "
       "3;\nmov.u32 %v2, %v1;\n",
       {"mov.u32 %v2, %v1;"},
       {"add.", "mul.", "%tmp_"}},
      {"unused memory accesses and side effects stay",
       "ld.global.u32 %tmp_b32_0, [%rd0];\nst.global.u32 [%rd0], %v1;\n@%p1 "
       "trap;\nbar.sync 0;\n"
       "atom.shared.add.u32 %r17, [counter], 1;\n",
       {"ld.global.u32", "st.global.u32", "@%p1 trap;", "bar.sync 0;", "atom.shared.add.u32"},
       {}},
      {"dead predicated calculation preserves old live value",
       "mov.u32 %tmp_b32_0, 7;\n@%p1 add.u32 %tmp_b32_0, %v1, 2;\nmov.u32 %v2, "
       "%tmp_b32_0;\n",
       {"mov.u32 %tmp_b32_0, 7;", "@%p1 add.u32 %tmp_b32_0, %v1, 2;", "mov.u32 %v2, %tmp_b32_0;"},
       {}},
      {"integer identities and self copies",
       "add.s32 %r14, %x1, 0;\nshl.b32 %r15, %r14, 0;\nmul.lo.u32 %r16, %r15, "
       "1;\n"
       "and.b32 %v1, %r16, 0xffffffff;\nmov.u32 %v2, %v2;\n",
       {"mov.b32 %v1, %x1;"},
       {"add.", "shl.", "mul.", "and.", "%r14", "%r15", "%r16", "%v2"}},
      {"FP identities must retain rounding and signed-zero semantics",
       "add.rn.f32 %v1, %v2, 0;\nmul.rn.f32 %v3, %v4, 1;\n",
       {"add.rn.f32", "mul.rn.f32"},
       {}},
      {"narrowing conversions and unpacking stay",
       "cvt.u8.u32 %tmp_u8_0, %v1;\nst.global.u8 [%rd0], %tmp_u8_0;\n"
       "mov.b32 {%tmp_b16_0, %tmp_b16_1}, %v2;\ncvt.f32.f16 %v3, %tmp_b16_0;\n"
       "cvt.f32.f16 %v4, %tmp_b16_1;\n",
       {"cvt.u8.u32", "mov.b32 {%tmp_b16_0, %tmp_b16_1}", "cvt.f32.f16"},
       {}},
      {"floating source cannot replace integer bit operand",
       "sqrt.rn.f32 %tmp_f32_0, %v1;\nmov.b32 %tmp_b32_0, %tmp_f32_0;\nxor.b32 "
       "%v2, %tmp_b32_0, 1;\n",
       {"xor.b32 %v2, %tmp_b32_0, 1;"},
       {"xor.b32 %v2, %tmp_f32_0"}},
      {"special registers are read at their use point",
       "mov.u32 %tmp_b32_0, %clock;\nmov.u32 %v1, %tmp_b32_0;\nadd.u32 %v2, "
       "%tmp_b32_0, 1;\n",
       {"mov.u32 %tmp_b32_0, %clock;", "add.u32 %v2, %tmp_b32_0, 1;"},
       {"add.u32 %v2, %clock"}},
      {"exact register token replacement",
       "mov.b32 %tmp_b32_1, %v1;\nadd.u32 %v2, %tmp_b32_1, %tmp_b32_10;\n",
       {"add.u32 %v2, %v1, %tmp_b32_10;"},
       {"%v10"}},
      {"branch to adjacent label with comments and aliases",
       "bra.uni L;\n// comment\nAlias:\nL:\nmov.u32 %v1, %v2;\n",
       {"Alias:\nL:", "mov.u32 %v1, %v2;"},
       {"bra"}},
      {"conditional and nonadjacent branches stay",
       "@%p1 bra L;\nmov.u32 %v1, %v2;\nbra M;\nL:\nmov.u32 %v3, %v4;\nM:\n",
       {"@%p1 bra L;", "bra M;"},
       {}},
      {"carry-setting operation is opaque",
       "add.cc.u32 %tmp_b32_0, %v1, 0;\naddc.u32 %v2, %v3, 1;\n",
       {"add.cc.u32 %tmp_b32_0, %v1, 0;", "addc.u32 %v2, %v3, 1;"},
       {}},
  };
}

} // namespace

int main() {
  const auto groups = {copy_cases(), safety_cases()};
  size_t count = 0;
  for (const auto &group : groups) {
    for (const auto &test : group)
      check_case(test);
    count += group.size();
  }
  std::cout << "ok ptx cleanup " << count << " cases\n";
}
