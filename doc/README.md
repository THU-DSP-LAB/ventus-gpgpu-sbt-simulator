# doc/ 索引（实现文档）

## 1. 当前优先阅读

若你想理解当前 PTX lowering 的下一阶段实施方向，建议按这个顺序读：

- `doc/PTX_LOWERING_MAIN_PROPOSAL.md`：当前主入口。统领 problems 1 / 3 / 4 的总体方向、边界、实施顺序与验证要求；文首已补充 replicated scalar-state 落地状态说明。
- `doc/PTX_LOWERING_REDUCTION_PLAN.md`：仍然活跃的专项规划。之所以继续保留在活跃文档集中，是因为其中的 problem 2（地址空间专门化）尚未被主提案取代。
- `doc/IMPLEMENTATION_CODEMAP.md`：目录/模块职责、关键数据结构与调用链（按源码入口落盘）。
- `doc/ventus-divergence-sgpr-analysis.md`：基于 `../llvm` 的参考分析，说明 Ventus LLVM 如何处理 `vbranch` / `join` 下的 SGPR/VGPR 有效性问题。

## 2. 其它活跃文档

- `doc/README.md`：本索引。
- `doc/IMPROVEMENT_PROPOSALS.md`：当前实现的主要问题清单与“向通用化演进”的改进方案（不改代码，仅建议）。
- `doc/PTX_XREG_QUALITY_REGRESSION_REPORT_2026-03-15.md`：`91bebb5` 相对 `3da36d6` 的 PTX / `ptxas` 质量回退评估；现已作为 replicated scalar-state 变更前 leader-lane 主线的历史背景材料。
- `lab/06_ptx_call_boundary_dead_state/README.md`：围绕“仅在 PTX call 边界出现的冷状态是否会被 `ptxas` 消去”的实验背景与目标。

历史阶段交接、旧设计稿与带日期的实测快照已归档在 `doc/archive/`（用于回溯当时的“as-built”结论与方案演进）。

## 3. 历史 PTX 设计/讨论参考（已归档）

以下文档已移入 `doc/archive/`，用于问题背景、方案展开与历史讨论回溯；它们不是当前实现方向的首选入口：

- `doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`：问题 3 的历史细化设计稿；当前主线已改用 `leader_lane` 术语，不再以 `owner lane` 作为公共口径。
- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`：问题 1 / 4 相关的历史细化设计稿；当前 `sbt/ptx_emit.cpp` 已采用其 `mutable/machine/runtime` 三层 blob ABI 主线。
- `doc/archive/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`：问题 4 的历史讨论纪要。

## 4. ISA/ABI 语义备忘

- `doc/ventus-isa/csr.md`：原型期涉及的 CSR 列表与备注（以仓库语义约定为准）。
- `doc/ventus-isa/vbranch_simtstack.md`：`setrpc/vbranch/join` 的 SIMT stack 语义。
- `doc/ventus-isa/others.md`：regext、mask、barrier、数值地址空间等杂项约定。

## 5. 归档（阶段交接 / 带日期快照）

- `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`：近期变更摘要与回归结果（2026-02-22）。
- `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`：阶段 4（PoCL/driver 集成、ptx_device + SBT PTX JIT）交接说明。
- `doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md`：SBT（Ventus ELF → decode → CFG verify → PTX）流水线梳理与实测结果（2026-02-19）。
- `doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`：历史 owner/leader 上下文设计稿。
- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`：历史 direct-call `value_blob` 设计稿。
- `doc/archive/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`：历史 direct-call 参数 ABI 讨论纪要。

## 6. 相关但不在 doc/ 的材料

- `README.md`：项目目标与命令用法（用户入口）。
- `lab/`：历史归档实验目录（不再作为当前实现入口）。
- `openspec/`：OpenSpec 变更提案与阶段 gate（已沉淀部分能力到 `openspec/specs/`；历史变更归档在 `openspec/changes/archive/`）。
