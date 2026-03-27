# 地址空间专门化问题说明

> 状态：`active future work`
>
> 这是历史 PTX lowering 问题列表中当前仍未完成的唯一主项：问题 2（普通访存的地址空间专门化）。
> problems 1 / 3 / 4 已由当前主线解决或实质性收敛；对应历史设计与计划已归档到 `doc/archive/`。

本文只描述**当前仍然存在的问题、边界与后续方向**，不再复用旧的四问题整体计划文档。
当前实现真相请以 `doc/IMPLEMENTATION_CODEMAP.md` 与 `sbt/ptx_emit.cpp` 为准。

## 1. 当前代码状态

当前普通标量/向量访存仍大量复用统一的数值地址映射模板：

- 先判断地址是否落在 shared 区间；
- 再判断是否落在 global 区间；
- 计算 shared pointer / global pointer；
- 最后执行真实 `ld/st`。

当前主要涉及的 lowering 入口包括：

- `emit_addr_map_and_ld_u32*`
- `emit_addr_map_and_st_u32*`
- `emit_addr_map_and_ld_u8_zext_u32*`
- `emit_addr_map_and_ld_u16_zext_u32*`

这仍是 `sbt/ptx_emit.cpp` 的 current 行为，不属于历史口径。

## 2. 问题描述

这一实现路径在 correctness 上可接受，但 PTX 质量偏差明显：

- 单条常见 `lw/sw/lb/lh/...` 往往会展开成多条与真实访存无关的模板指令；
- 即使地址显然来自 `x2/x8 + imm` 的栈/LDS 访问，也常走完整模板；
- 即使地址显然来自 `x10` 参数区或 `auipc/lui + imm` 形成的 Global 地址，也常走完整模板；
- Rodinia/PoCL 中高频出现的普通访存会因此承担稳定且系统性的 PTX 膨胀。

换句话说，当前 remaining issue 不是“访存语义错误”，而是“过多可静态判定的地址仍被当作 Unknown 处理”。

## 3. 当前边界

后续处理这个问题时，当前边界应保持不变：

- 不引入 silent fallback，也不以模糊降级路径掩盖地址分类失败；
- replicated active-lane scalar state、structured divergence、direct-call value ABI 不应被重新设计；
- 数值地址空间映射、PDS 语义与当前 runtime 约定保持兼容；
- 无法静态判定的地址仍应显式走通用模板，而不是猜测地址空间。

## 4. Non-Goals

本文不覆盖以下事项：

- 不重新讨论 problems 1 / 3 / 4 的总体方案，它们已进入历史归档；
- 不把地址空间专门化扩展成新的 ABI 设计文档；
- 不把 PDS、scalar-state、divergence、direct-call 协议一起重写；
- 不把“未来可能可做的优化”写成当前已冻结的实施承诺。

## 5. 后续方向

当前更合理的方向是把问题收敛为“地址来源可判定时的专门化 lowering”：

- 对常见地址表达式做静态分类，例如 `SharedKnown`、`GlobalKnown`、`Unknown`；
- 优先覆盖 `x2/x8` 派生栈/LDS、`x10` 派生参数区、`auipc/lui + imm` 形成的 Global 区；
- 仅在地址空间可证明时发 `.shared` / `.global` 专门化 PTX；
- 对不能证明的情况继续保留当前通用模板。

这里记录的是 future direction，不是已冻结的实现计划。

## 6. 验证口径

后续若推进该问题，至少应比较：

- PTX 指令总数；
- 普通 `lw/sw/lb/lh/...` 对应模板长度；
- `ptxas -v` 的寄存器、stack、spill、compile time；
- Rodinia / PoCL 常见 kernel 的 compile-first 结果是否保持稳定。

## 7. 历史材料

若需要回看此前完整的四问题方案与讨论，可参考：

- `doc/archive/PTX_LOWERING_MAIN_PROPOSAL.md`
- `doc/archive/PTX_LOWERING_REDUCTION_PLAN.md`
- `doc/archive/PTX_XREG_QUALITY_REGRESSION_REPORT_2026-03-15.md`
