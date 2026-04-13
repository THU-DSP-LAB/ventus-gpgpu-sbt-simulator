## Context

本设计描述的是 **target behavior**，不是当前已实现行为。

本 change 依赖的 canonical current docs/specs 有：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/project.md`
- `openspec/specs/inst-support/spec.md`
- archived change `support-custom-instructions` as historical context only
- `doc/mma/LOWERING_ARCHITECTURE.md`

本 change 同时依赖一份跨两个 active changes 的 shared active baseline：

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`
- `doc/mma/LOWERING_ARCHITECTURE.md`

MMA 被拆成独立 change 的直接原因是：它的主要难点不在普通 custom decode，而在 shape/layout/type 与 PTX native MMA 之间的真实映射能力。当前已知背景包括：

- `doc/CUSTOM_INSTRUCTION_INPUT.md` 列出了多种 Ventus MMA shape/type 组合；
- 本地 `ptxas` 探针已经证明其中一部分 shape 不能直接作为 PTX native MMA shape 使用；
- `tf32` / `bf16` MMA 需要高于当前 `sm_75` 主线的 PTX/SM target，但 shared baseline 已收敛到 `sm_89`；
- 即使某个组合存在 PTX native MMA，也仍需证明 Ventus 的寄存器窗口与 PTX fragment 寄存器排布能够稳定对应。
- 对首发 `native-mma-sync` 子集，本地 `ptxas 13.1` 探针还表明 `row.col` 可接受，而 `row.row` / `col.row` / `col.col` 不在当前 native 接受面中。

因此，本 change 不能从“先实现，再归纳支持范围”出发，而必须先写 support matrix，再按矩阵实现。

另外，MMA 所需 target、semantic oracle 与首发支持边界都不再由本 change 单独决定，而是先冻结在 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中，再由本 change 在其边界内展开 support matrix；而 `VGPR window -> logical tile -> PTX fragment tuple` 的 lowering 架构则由 `doc/mma/LOWERING_ARCHITECTURE.md` 作为长期维护的 active 文档冻结。

## Goals / Non-Goals

**Goals:**

- 为 Ventus MMA 指令建立 explicit support matrix。
- 定义支持矩阵中每个组合的 lowering 路径和所需 PTX target。
- 明确 unsupported 组合的 fail-fast 行为与诊断要求。
- 为 supported MMA subset 规划 compile-first 与语义验证闭环。
- 冻结一套不会干扰既有 non-MMA lowering 的 MMA 专属 lowering 架构。
- 在 shared baseline 文档已冻结的 `.version 7.8` / `sm_89` / oracle / MMA 首发边界之上展开 support matrix，但不单独决定 project-wide baseline。

**Non-Goals:**

- 不在本 change 中重新承担 non-MMA custom 指令的 decode/lowering 工作。
- 不承诺原始文档中的所有 MMA shape/type 组合都能被支持。
- 不引入 silent fallback、mock 成功路径或“近似替代另一种 MMA”的降级行为。

## Decisions

### 1. MMA is specified as a matrix, not a flat list

本 change 先建立矩阵，再实现。矩阵的最小维度包括：

- `shape`
- `alayout`
- `blayout`
- A/B type
- C/D type
- required PTX target
- lowering path
- support status

只有矩阵中明确标记 supported 的组合，才进入实现与验证。

本 change 进一步冻结 matrix 的记录方式：

- `doc/mma/LOWERING_ARCHITECTURE.md` 保存 canonical master matrix
- master matrix 只列出 committed first-batch entries
- frontend 已暴露但未承诺的组合进入单独 inventory，状态只能是 `deferred` / `research` / `TODO`

也就是说，本 change 需要把未承诺项列出来，但不能假装它们已经有正确 lowering 方案。

**Alternatives considered:**

- 按原始文档的平面指令列表直接逐条实现：Rejected，因为这会掩盖 shape/type 组合之间的根本差异。

### 2. Native PTX MMA is preferred, but not assumed

对每个矩阵组合，优先评估是否存在可由 `ptxas` 接受的 native PTX MMA 形式。native PTX 候选路径不只包括 `mma.sync`，也包括 `wmma`。如果存在，再继续做寄存器窗口与 fragment mapping；如果不存在，则评估是否存在稳定的分解路径；若两者都没有，则明确列为 unsupported。

这意味着“native-first”是优先级，不是先验前提。

**Alternatives considered:**

- 默认所有 Ventus MMA 都能用单条 PTX native MMA 表达：Rejected，因为现有探针已否定这一假设。
- 对 unsupported 组合静默替换为最接近的 PTX native shape：Rejected，因为这会制造难以发现的语义偏差。

### 3. Logical-tile mapping is the semantic core

本 change 不允许直接从 Ventus VGPR window 跳到 PTX fragment tuple。MMA lowering 必须显式经过三层：

- `VGPR window`
- `logical tile`
- `PTX fragment tuple`

其中 `logical tile` 层是语义核心，必须与 Spike 现有的 `A/B/C/D` 装载回写行为对齐；PTX tuple 只负责消费已经定义好的逻辑 tile，而不能重新解释 Ventus 语义。

**Alternatives considered:**

- 直接在 emitter 里按 shape 经验性拼 PTX tuple：Rejected，因为这会制造 compile-first 可过但语义错误的高风险路径。

本 change 进一步冻结 first-batch 的 formal mapping 规则在 `doc/mma/LOWERING_ARCHITECTURE.md` 中：

- `VGPR window summary`
- `VGPR window / lane-slot -> logical tile`（以 Spike `load_matrix_a/load_matrix_b_block/load_matrix_c_block/store_matrix_d` 的索引公式为 canonical source）
- `logical tile -> PTX fragment tuple`（无论 direct-native 还是 split-`n` composite，PTX fragment ABI instantiation 都只能从 PTX operand contract 冻结，禁止拍脑袋；其中 split-`n` 的 block partition 仍必须完全对齐 Spike）

### 4. Register-window / fragment mapping is a first-class design artifact

对于每个 supported MMA 组合，必须显式说明：

- Ventus 指令中的寄存器窗口如何切分 A/B/C/D；
- `VGPR window -> logical tile` 的规则；
- PTX 侧需要哪些 fragment 寄存器；
- `logical tile -> PTX operand fragment` 的规则；
- 结果如何回写到 Ventus 约定的寄存器窗口。

如果这一步无法给出稳定规则，则该组合即使存在 PTX native shape，也不能被标记为 supported。

对于 first-batch committed `row.col` families，这些规则必须以 `doc/mma/LOWERING_ARCHITECTURE.md` 的 formal mapping section 为准，OpenSpec artifacts 不应再重复另一套不同表述。

**Alternatives considered:**

- 先用经验性寄存器拼接写 emitter，之后再补文档：Rejected，因为 MMA 的主要风险恰恰就在 fragment mapping。

### 5. The first batch is `row.col`, but it is not direct-native only

本 change 的首发批次限定为 `row.col`，但不再限定为“只有 direct-native”。首发批次包含两类：

- direct-native `m16n8k16` / `m16n8k8`
- committed `split-n` composite `m16n16k16` / `m16n16k8`

这里的 composite 不是一般性的“任意分解”，而是冻结成一条非常具体的 contract：

- `m16n16*` 只允许沿逻辑 `n` 维切成两个 `n=8` subtiles
- 两个 sub-ops 共享同一逻辑 `A`
- `B/C/D` 按 `n=[0..7]` 和 `n=[8..15]` 两半切分

`m8*`、非 `row.col` 与 `wmma` 仍然在首发批次之外。

**Alternatives considered:**

- 把首发批次继续限制为 4 个 direct-native 项：Rejected，因为这无法覆盖当前 upstream `ventus-pytorch` 已真实使用的 `m16n16* row.col` kernel family。

### 6. MMA-specific semantics and decode ownership stay in this change

`support-custom-instructions` 可以拥有 shared custom decode framework，但 MMA 的 `0x0A` 指令语义、matrix、metadata 字段定稿、supported/unsupported 边界和正式 decode ownership 都属于本 change。

同时，本 change 必须遵守 shared baseline 文档中已冻结的首发边界：首发批次只限于 `row.col` 的 direct-native `m16n8*` 与 committed `split-n` composite `m16n16*`；`wmma` 属于 active research target；其它 composite 形式不进入首发承诺。

这样可以避免两个 active changes 同时声称拥有 MMA decode 语义。

本 change 同时冻结 decode metadata contract：

- `DecodedInst.custom.family = CustomFamily::Mma` 只承担 family ownership 标识
- MMA 专属字段进入独立 `MmaInstInfo`
- `MmaInstInfo` 是 `DecodedInst` 的 first-class 成员，而不是 emitter 重新解析 raw bits
- 不允许把 `shape/layout/type/window` 信息继续塞进面向 non-MMA 的 `CustomInstInfo`

首批 `MmaInstInfo` 至少需要携带：

- `shape`
- `a_layout`
- `b_layout`
- `ab_type`
- `cd_type`
- `spike_a_column_layout`
- `spike_b_row_layout`
- `rd_base` / `rs1_base` / `rs2_base`
- `a_regs_per_thread` / `b_regs_per_thread` / `c_regs_per_thread`
- `wide_ab`
- `support_class`

### 7. Supported MMA semantic validation uses Spike-backed comparison

对于首发批次中标记 supported 的 MMA 组合，canonical semantic oracle 是 Spike-backed OpenCL buffer comparison。

验证资产必须前置，而不是等 PTX lowering 基本完成后再补。实施顺序要求至少包含一个前置 bring-up 步骤：

- 先选取至少一个首批 committed family 的最小 MMA observable microtest；
- 先在 Spike-backed 路径上跑通该测例，确认输入布局、寄存器窗口与输出观察方式；
- 再复用同一测例进入后续 PTX compile-first 与 Spike-vs-PTX semantic gate。

这样做的目的不是“提前证明 PTX 正确”，而是先冻结可观察的语义验证载体，避免实现阶段把验证资产变成事后补写、并让 layout/writeback 争议拖到 emitter 已经成形之后才暴露。

repository-local helper model 可以在 bring-up 阶段保留为附加 cross-check，但它不再单独定义长期 contract。

**Alternatives considered:**

- 继续把 MMA 的 canonical oracle 保持为非 Spike 路径：Rejected，因为当前 Spike 已经提供真实 MMA 执行语义，并且 active lowering 架构明确以 Spike 的 tile/load/store 语义为基准。

### 8. PTX operand ABI is instantiated through explicit native ABI descriptors

首批 PTX operand tuple 不能再停留在“等实现时再看”的状态。本 change 现在冻结一条明确实现策略：

- `VGPR window -> logical tile` 由 Spike 语义冻结
- `logical tile -> PTX tuple` 由显式 `PtxMmaAbiDesc` 冻结
- 每个 emitted native `mma.sync` 必须绑定一个 `PtxMmaAbiKey`
- direct-native 与 split-`n` composite 共用同一套 tuple materialization helpers

对首批 committed families，native ABI key 限定为：

- `M16N8K16_F16_F16`
- `M16N8K16_F16_F32`
- `M16N8K16_BF16_F32`
- `M16N8K8_TF32_F32`

这些 descriptor 至少需要冻结：

- native PTX form
- `A/B/C/D` tuple arity per lane
- packed-16 vs wide-32 element mode
- operand tuple materialization helpers
- result merge helper

这条 contract 的含义是：即使 direct-native family 的寄存器数与 PTX tuple arity 看起来一致，实现上也必须经过显式 tuple materialization 层；只有在代码和验证证明“它恰好退化成 identity copy”后，才能把它当成实现优化，而不是先验 contract。

**Alternatives considered:**

- 继续把 PTX tuple ABI 留成实现期 TODO：Rejected，因为这会把最关键的 lowering 接口留给代码反向塑形。
- 对 direct-native 默认使用 VGPR window 顺序，对 composite 再单独 special-case：Rejected，因为这会让 direct-native 和 composite 拥有两套语义入口，增加错配风险。

## Risks / Trade-offs

- **Support subset 过窄风险**：最终 supported matrix 可能只覆盖原始文档中的一部分组合，这会低于最初直觉预期。
- **Shared-foundation 依赖风险**：本 change 依赖 `support-custom-instructions` 提供的 custom decode framework，以及 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中冻结的共享前提；若前者设计收口变化，本 change 需要同步更新。
- **验证成本风险**：MMA 的 compile-first 只能证明 PTX 形态成立，不能证明 fragment mapping 正确，因此语义验证成本显著高于普通 custom 指令。
- **架构分层成本风险**：为避免污染既有 lowering，本 change 需要引入 MMA 专属 metadata / lowering plan / tile mapping 层，这会增加前期设计与实现成本。
- **Target requirement risk**：MMA 组合可能要求的 PTX target 与 non-MMA change 选定的 baseline 不完全一致，需要在矩阵中逐项明确。
