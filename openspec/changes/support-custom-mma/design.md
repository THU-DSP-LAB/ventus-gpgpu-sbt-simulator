## Context

本设计描述的是 **target behavior**，不是当前已实现行为。

本 change 依赖的 canonical current docs/specs 有：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/project.md`
- `openspec/specs/inst-support/spec.md`
- active change `support-custom-instructions`

本 change 同时依赖一份跨两个 active changes 的 shared active baseline：

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`

MMA 被拆成独立 change 的直接原因是：它的主要难点不在普通 custom decode，而在 shape/layout/type 与 PTX native MMA 之间的真实映射能力。当前已知背景包括：

- `doc/CUSTOM_INSTRUCTION_INPUT.md` 列出了多种 Ventus MMA shape/type 组合；
- 本地 `ptxas` 探针已经证明其中一部分 shape 不能直接作为 PTX native MMA shape 使用；
- `tf32` / `bf16` MMA 需要更高 PTX/SM target；
- 即使某个组合存在 PTX native MMA，也仍需证明 Ventus 的寄存器窗口与 PTX fragment 寄存器排布能够稳定对应。
- 对首发 `native-mma-sync` 子集，本地 `ptxas 13.1` 探针还表明 `row.col` 可接受，而 `row.row` / `col.row` / `col.col` 不在当前 native 接受面中。

因此，本 change 不能从“先实现，再归纳支持范围”出发，而必须先写 support matrix，再按矩阵实现。

另外，MMA 所需 target、semantic oracle 与首发支持边界都不再由本 change 单独决定，而是先冻结在 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中，再由本 change 在其边界内展开 support matrix。

## Goals / Non-Goals

**Goals:**

- 为 Ventus MMA 指令建立 explicit support matrix。
- 定义支持矩阵中每个组合的 lowering 路径和所需 PTX target。
- 明确 unsupported 组合的 fail-fast 行为与诊断要求。
- 为 supported MMA subset 规划 compile-first 与语义验证闭环。
- 在 shared baseline 文档已冻结的 `.version 7.8` / `sm_90` / oracle / MMA 首发边界之上展开 support matrix，但不单独决定 project-wide baseline。

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

**Alternatives considered:**

- 按原始文档的平面指令列表直接逐条实现：Rejected，因为这会掩盖 shape/type 组合之间的根本差异。

### 2. Native PTX MMA is preferred, but not assumed

对每个矩阵组合，优先评估是否存在可由 `ptxas` 接受的 native PTX MMA 形式。native PTX 候选路径不只包括 `mma.sync`，也包括 `wmma`。如果存在，再继续做寄存器窗口与 fragment mapping；如果不存在，则评估是否存在稳定的分解路径；若两者都没有，则明确列为 unsupported。

这意味着“native-first”是优先级，不是先验前提。

**Alternatives considered:**

- 默认所有 Ventus MMA 都能用单条 PTX native MMA 表达：Rejected，因为现有探针已否定这一假设。
- 对 unsupported 组合静默替换为最接近的 PTX native shape：Rejected，因为这会制造难以发现的语义偏差。

### 3. Register-window / fragment mapping is a first-class design artifact

对于每个 supported MMA 组合，必须显式说明：

- Ventus 指令中的寄存器窗口如何切分 A/B/C/D；
- PTX 侧需要哪些 fragment 寄存器；
- lane-level 数据排列如何进入 PTX operand fragment；
- 结果如何回写到 Ventus 约定的寄存器窗口。

如果这一步无法给出稳定规则，则该组合即使存在 PTX native shape，也不能被标记为 supported。

**Alternatives considered:**

- 先用经验性寄存器拼接写 emitter，之后再补文档：Rejected，因为 MMA 的主要风险恰恰就在 fragment mapping。

### 4. MMA-specific semantics and decode ownership stay in this change

`support-custom-instructions` 可以拥有 shared custom decode framework，但 MMA 的 `0x0A` 指令语义、matrix、metadata 字段定稿、supported/unsupported 边界和正式 decode ownership 都属于本 change。

同时，本 change 必须遵守 shared baseline 文档中已冻结的首发边界：初始承诺子集只限于 `native-mma-sync row.col`，`wmma` 属于 active research target，composite MMA lowering 不进入首发承诺。

这样可以避免两个 active changes 同时声称拥有 MMA decode 语义。

## Risks / Trade-offs

- **Support subset 过窄风险**：最终 supported matrix 可能只覆盖原始文档中的一部分组合，这会低于最初直觉预期。
- **Shared-foundation 依赖风险**：本 change 依赖 `support-custom-instructions` 提供的 custom decode framework，以及 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中冻结的共享前提；若前者设计收口变化，本 change 需要同步更新。
- **验证成本风险**：MMA 的 compile-first 只能证明 PTX 形态成立，不能证明 fragment mapping 正确，因此语义验证成本显著高于普通 custom 指令。
- **Target requirement risk**：MMA 组合可能要求的 PTX target 与 non-MMA change 选定的 baseline 不完全一致，需要在矩阵中逐项明确。
