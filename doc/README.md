# doc/ 索引（实现文档）

## 状态图例

- `current`：当前实现真相或当前 contract
- `active`：仍在推进的共享前置决策、剩余问题或未来工作
- `historical`：历史设计、历史计划、历史回归结论
- `legacy`：仍有参考价值，但已不再是当前入口或回归基线

维护约定：
- 长期维护文档优先在文首标明状态与角色；不要把“最近一次提交 hash/更新时间列表”当作主维护机制，文档新鲜度应通过正文口径、索引位置和 Git 历史共同体现。
- `doc/` 根目录优先保留长期维护入口：`current` 文档，以及少量跨多个 change 仍成立的 `active` 专题。
- 同一主题在 `doc/` 根目录只保留一个 active 入口；若是某次 change 的阶段性 proposal/design/tasks，应进入 `openspec/changes/` 而不是继续留在 `doc/` 根目录竞争入口。

## 1. 当前优先阅读（按“先现状、再设计、后背景”）

若你想理解当前 PTX lowering 主线与回归口径，建议按这个顺序读：

- `README.md`：用户入口、构建命令、回归入口、文档导航。
- `doc/IMPLEMENTATION_CODEMAP.md`：当前实现真相（as-built）。目录/模块职责、关键数据结构与调用链，以及 current PTX 固定槽位 / `%tmp*` scratch ownership 口径。
- current lowering authority 也以 `doc/IMPLEMENTATION_CODEMAP.md`、`sbt/emit_descriptor.hpp`、`sbt/instruction_metadata.cpp`、`sbt/control_semantics.cpp`、`sbt/cfg.cpp`、`sbt/cfg_verify.cpp` 与 `sbt/ptx_emit.cpp` + `sbt/ptx_emit_internal.hpp` + `sbt/ptx_emit_{control,scalar,vector,custom,mma_lowering}.cpp` 为准：当前 supported emit path 与 main-pipeline control-flow path 都已 descriptor-driven，`DecodedInst.name` 只保留 external mnemonic contract；decode/shared-metadata lookup 仍保留内部 name-keyed metadata 组织。
- current instruction metadata 口径也以 `doc/IMPLEMENTATION_CODEMAP.md`、`sbt/instruction_metadata.cpp`、`openspec/specs/inst-support/spec.md` 与 `openspec/specs/replicated-scalar-state/spec.md` 为准：不要再把 decode / CFG verify / emitter 的字符串 suffix 推断当作当前事实源。
- `openspec/README.md`：OpenSpec 状态分层、当前 specs 与 archive 的使用方式。
- `openspec/specs/global-address-space/spec.md`：当前 PTX ordinary address mapping / single-Global / VMM backing contract。
- `openspec/specs/ptx-lowering-modularity/spec.md`：当前 PTX emitter shared core / domain lowering / dispatcher precedence / structural validation 合同。
- `openspec/specs/ptx-temp-register-allocation/spec.md`：当前 PTX emitter 固定槽位与 `%tmp*` scratch ownership contract。
- `openspec/specs/replicated-scalar-state/spec.md`：当前 PTX lowering contract 的核心规格。
- `openspec/specs/ptx-call-prototype/spec.md`：多函数 PTX helper prototype / value ABI 合同。
- `doc/ADDRESS_SPACE_SPECIALIZATION.md`：当前仍活跃的剩余问题说明（问题 2）。

若你要看仍然活跃的前瞻专项，再读：

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`：custom 指令拆分阶段留下的 active 共享前置基线；冻结 `.version 7.8` / `sm_89`、family-scoped oracle policy、non-MMA 已落地语义和 MMA 首发批次边界，并明确 `bf16x2` 的 mixed native/composite lowering 边界。相关 split changes 已归档，但该文档仍作为后续 blocked/deferred MMA 扩展的长期前置参考。
- `doc/CUSTOM_INSTRUCTION_INPUT.md`：custom 指令输入材料的受管副本；供 shared baseline 与 MMA 架构文档引用，但不覆盖 current spec。
- `doc/mma/LOWERING_ARCHITECTURE.md`：Ventus MMA 的 active lowering 架构文档；冻结 canonical MMA matrix、`VGPR window -> logical tile -> PTX fragment tuple` 的分层模型、`MmaInstInfo` / native PTX ABI descriptor contract，以及“不干扰既有 non-MMA lowering”的接入约束。当前首批 committed `row.col` MMA 子集已作为 current 行为落地；该文档继续承担 blocked/deferred/research MMA families 与剩余架构边界的 active 入口。
- `doc/ventus-divergence-sgpr-analysis.md`：基于 `../llvm` 的参考分析，说明 Ventus LLVM 如何处理 `vbranch` / `join` 下的 SGPR/VGPR 有效性问题。

口径提醒：custom 指令当前已包含 landed 的 non-MMA 子集与首批 committed `row.col` MMA 子集，其中 `fp16 -> fp16` 当前支持族仅限 `m16n8k16 row.col` 与 `m16n16k16 row.col`。当前已支持 MMA family 的默认语义 gate 统一为 `Spike / sbtsim PTX / CPU reference` 三方检查，并覆盖较小/较大两档随机样本。deferred/research MMA families、非 current `fp16 -> fp16` 组合及更宽的矩阵/架构讨论仍保留在 `active` 长期文档中，而不再由单独 active change 承载。

## 2. 当前事实与 contract

- `doc/IMPLEMENTATION_CODEMAP.md`：当前代码口径。
- 当前 PTX 寄存器 ownership 以 `doc/IMPLEMENTATION_CODEMAP.md` 与 `sbt/ptx_emit.cpp` 为准：固定 machine/runtime/control 槽位与函数级 `%tmp*` scratch 已显式分离。
- `openspec/specs/global-address-space/spec.md`：当前 Global 地址空间 / driver VMM backing 合同。
- `doc/ADDRESS_SPACE_SPECIALIZATION.md`：当前仍活跃的地址空间专门化问题说明。
- `openspec/specs/replicated-scalar-state/spec.md`：当前标量状态/分歧/调用合同。
- `openspec/specs/ptx-call-prototype/spec.md`：当前 helper prototype 合同。
- `openspec/specs/ptx-lowering-modularity/spec.md`：当前 PTX emitter 模块化结构合同。
- `openspec/specs/ptx-temp-register-allocation/spec.md`：当前 PTX scratch ownership / `%tmp*` virtual temp 合同。
- `openspec/specs/inst-support/spec.md`：当前指令支持与验证合同。
- current Spike-backed 指令形态 / uniform-transfer metadata 以 `openspec/specs/inst-support/spec.md` 与 `sbt/instruction_metadata.cpp` 为准。
- current name-dependence 边界以 `doc/IMPLEMENTATION_CODEMAP.md`、`openspec/specs/inst-support/spec.md` 与 `openspec/changes/archive/2026-04-18-reduce-lowering-name-dependence/inventory.md` 共同记录：current main-pipeline control-flow authority 已从 CFG / verify / direct-call scan 中移除，remaining internal name-keyed site 主要是 decode/shared-metadata lookup 的实现组织；emitter 结构收敛本身也已归档，见 `openspec/changes/archive/2026-04-25-modularize-ptx-emit-lowering/`。
- `openspec/specs/sbt-rodinia-bringup/spec.md`：当前 Rodinia bring-up / fail-fast 边界。
- `openspec/specs/build-time-spike-pattern-subset/spec.md`：当前 Spike pattern 子集生成合同。
- current scalar execution classification 以 `openspec/specs/replicated-scalar-state/spec.md` 与 `sbt/instruction_metadata.cpp` 为准；未分类 scalar 当前是显式失败，不存在默认 `UniformPure` fallback。
- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`：`active` shared baseline（非 current as-built）；保留 custom split 的共享前提，并继续约束后续 blocked/deferred MMA 扩展的基线选择。
- `doc/mma/LOWERING_ARCHITECTURE.md`：`active` MMA lowering 架构合同（非 current as-built）；规定 canonical MMA matrix、首批 `row.col` MMA 的分层 lowering 模型、decode metadata contract 和 composite `split-n` 路径，并为后续 blocked/deferred/research family 保留统一架构入口。

## 3. 历史 PTX 设计/规划（已归档）

- `doc/archive/PTX_LOWERING_MAIN_PROPOSAL.md`：历史主提案；problems 1 / 3 / 4 已被当前实现主线解决，problem 2 已拆到 `doc/ADDRESS_SPACE_SPECIALIZATION.md`。
- `doc/archive/PTX_LOWERING_REDUCTION_PLAN.md`：历史四问题缩减计划；现仅保留为归档背景，不再作为当前 active 计划。
- `doc/archive/PTX_XREG_QUALITY_REGRESSION_REPORT_2026-03-15.md`：replicated scalar-state 之前 leader-lane 主线的质量回退评估。
- `doc/archive/IMPROVEMENT_PROPOSALS.md`：历史工程化改进建议汇总；它是 mixed engineering backlog 快照，不是当前实现或当前计划入口。

## 4. 历史 PTX 设计/讨论参考（已归档）

以下文档已移入 `doc/archive/`，用于问题背景、方案展开与历史讨论回溯；它们不是当前实现方向的首选入口，也不应覆盖 `doc/IMPLEMENTATION_CODEMAP.md` / `openspec/specs/*` 的 current 口径：

- `doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`：问题 3 的历史细化设计稿；当前主线已改用 `leader_lane` 术语，不再以 `owner lane` 作为公共口径。
- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`：问题 1 / 4 相关的历史细化设计稿；当前 `sbt/ptx_emit.cpp` 已采用其 `mutable/machine/runtime` 三层 blob ABI 主线。
- `doc/archive/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`：问题 4 的历史讨论纪要。

## 5. ISA/ABI 语义备忘

- `doc/ventus-isa/csr.md`：原型期涉及的 CSR 列表与备注（以仓库语义约定为准）。
- `doc/ventus-isa/vbranch_simtstack.md`：`setrpc/vbranch/join` 的 SIMT stack 语义。
- `doc/ventus-isa/others.md`：regext、mask、barrier、数值地址空间等杂项约定。

## 6. 归档（阶段交接 / 带日期快照）

- `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`：近期变更摘要与回归结果（2026-02-22）。
- `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`：阶段 4（PoCL/driver 集成、ptx_device + SBT PTX JIT）交接说明。
- `doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md`：SBT（Ventus ELF → decode → CFG verify → PTX）流水线梳理与实测结果（2026-02-19）。
- `doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`：历史 owner/leader 上下文设计稿。
- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`：历史 direct-call `value_blob` 设计稿。
- `doc/archive/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`：历史 direct-call 参数 ABI 讨论纪要。

## 7. 相关但不在 doc/ 的材料

- `README.md`：项目目标与命令用法（用户入口）。
- `lab/`：历史归档实验目录（不再作为当前实现入口）。
- `lab/07_fp16_mma_ptx_probe/`：`historical` 的 `fp16 -> fp16` MMA 前期探针实验；其结论已吸收进 current spec / tests / tooling，不再作为当前 contract 或统一回归入口。
- `openspec/`：OpenSpec 当前 contract、活跃 change 与历史归档；具体分层见 `openspec/README.md`。
