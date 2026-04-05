## Why

MMA 是这批 custom 指令里风险最高、与 PTX backend 耦合最深的一组能力。把它继续和 `shuffle/vcvt/packed/SFU` 混在同一个 change 里，会掩盖两个事实：

- MMA 的核心问题不是“再补几个 decode/lowering case”，而是先要证明 Ventus `shape/layout/type` 与 NVIDIA PTX fragment/native MMA 之间到底有哪些真实可支持组合。
- 本地 `ptxas` 探针已经表明，`doc/CUSTOM_INSTRUCTION_INPUT.md` 中列出的部分 MMA shape 不能直接作为 PTX native MMA shape 使用，因此“文档列出即默认可实现”这一假设不成立。

相比之下，non-MMA custom 指令已经可以形成一条较清晰的实现主线。因此 MMA 应拆成独立 active change，以显式 support matrix 驱动实现，而不是继续作为前一个 change 的“第二阶段待办”。

同时，MMA 与 non-MMA 都会共同影响 PTX baseline 选择，因此在进入任一 custom change 的真正实现之前，必须先冻结一份 shared active baseline 文档：`doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`，避免 change 1 / change 2 分别推动不同 baseline。

## What Changes

- 新建独立 active change `support-custom-mma`，专门负责 MMA 的 canonicalization、support matrix、decode/lowering 设计与验证计划。
- 明确本 change 与 `support-custom-instructions` 的边界：
  - `support-custom-instructions` 负责 non-MMA custom 指令，以及 shared custom decode framework 的主线基础设施；
  - 本 change 只负责 MMA 的专属元数据、`0x0A` 语义、寄存器窗口/fragments 映射、support matrix 和验证闭环。
- 明确本 change 需要遵守 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中冻结的 `.version 7.8` / `sm_89` baseline、shared oracle policy，以及 MMA 首发只承诺 `native-mma-sync` `row.col` 子集的边界。
- 将 MMA 支持定义为“显式支持子集 + 显式 fail-fast unsupported 子集”，而不是承诺一次性覆盖原始文档中的全部组合。

## Capabilities

### New Capabilities

- **Explicit MMA support matrix**：为 Ventus MMA 指令建立 shape/layout/type 到 PTX lowering 的正式支持矩阵。
- **MMA compile-supported subset**：对矩阵中标记 supported 的组合，进入 decode + PTX emit + `ptxas` compile-first 路径。
- **Explicit unsupported diagnostics for MMA**：对矩阵外组合保持明确的 fail-fast 诊断，而不是静默替代或降级。

### Modified Capabilities

- **Custom-instruction planning boundary**：custom 指令支持主题从单一 change 拆分为 non-MMA 与 MMA 两条 active 轨道，减少 shared plan 中的职责混叠。

## Impact

- MMA 的实施节奏将独立于 non-MMA custom 指令，不再阻塞前者落地。
- 当前 contract 将不再允许“把 MMA 视作 non-MMA change 后续阶段自然完成”的模糊表述，而会要求显式 support matrix。
- 本 change 预计会依赖 `support-custom-instructions` 提供的 shared custom decode framework，以及 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 的 shared baseline 结论；若该基础设施发生变化，本 change 也需同步调整。

## Documentation Impact

本 change 落地时至少需要检查并更新以下文档：

- `README.md`
- `doc/README.md`
- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`

本 change 不 supersede `support-custom-instructions`；它与后者共同组成 custom 指令支持的两条 active change，其中本 change 只承担 MMA 范围。
