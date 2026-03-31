## Why

`doc/CUSTOM_INSTRUCTION_INPUT.md` 描述了一批计划中的 Ventus custom 指令，但它当前仍只是输入材料，不是 `openspec/specs/` 下的 canonical contract。当前实现也存在几处和这批指令直接冲突的现实边界：

- 前端解码当前主要依赖 Spike pattern 子集；若新增指令不在 `../spike/riscv/encoding.h` 中，`--require-known` 路径无法仅靠现有 whitelist/pattern 机制接入。
- 当前 PTX emitter 固定输出较低 PTX 版本，并且仓库默认 compile-first / 回归口径仍以 `sm_75` 为主；这与 `bf16x2`、`tf32`、以及 MMA 所需的 PTX 能力不匹配。
- 项目 current domain context 明确 `v0` 是普通向量寄存器，而输入材料里一度存在把 `vm`/`m` 位写成 `v0-mask` 语义的错误表述；如果不先 canonicalize，后续实现很容易把错误语义写死进 decode/lowering。
- MMA 指令的 Ventus `shape/layout/type` 组合并不能假设与 NVIDIA PTX `mma.sync` 一一对应；本地 `ptxas` 探针已经表明，文档列出的部分 shape 不能直接作为 PTX native MMA shape 使用。

在用户已明确接受三项前提后：

- 允许在 Spike 之外新增本地解码路径；
- 允许直接提高基础 SM 要求；
- 明确 `v0` 不是 mask 寄存器，non-MMA custom 指令中的 `vm`/`m` 编码位当前也不引入独立 mask 语义；

最合理的推进方式不再是“把全部指令视作同一批实现”，而是拆成两个 active changes：

1. `support-custom-instructions`：只负责除 MMA 之外的所有 custom 指令；
2. `support-custom-mma`：单独负责 MMA，并以显式 support matrix 管理风险。

但在进入这两个 change 的真正实现之前，还必须先完成一个 shared prerequisite：

- 将跨两个 active changes 的共享前置决策冻结在 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`；
- 避免出现 change 1 先按某个 baseline 落地、change 2 再迫使全项目二次抬升的重复收敛。

## What Changes

- 将当前零散定义收敛为多个边界清晰的 active OpenSpec changes，而不是继续把所有 custom 指令打包在一个 change 里。
- 本 change 只实现非 MMA 指令支持：
  - `shuffle`
  - `vcvt`
  - packed `f16x2` / `bf16x2`
  - `fp32` / packed `f16x2` / packed `bf16x2` SFU
- 调整 current contract：
  - custom 指令支持不再被“必须来自 Spike whitelist”这一隐含前提绑定；
  - custom 非 MMA 指令不引入 `v0` mask 语义，相关 `vm`/`m` 位仅保留为编码位；
  - custom 指令相关的 PTX baseline（`.version 7.8` / `sm_89`）、oracle、packed 语义与 MMA 首发边界，必须先遵守 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`，而不是由本 change 单独隐式决定。
  - packed `bf16x2` 支持在 shared `sm_89` baseline 下保留，但其 lowering 不再假设存在 `sm_90`-only native `add/mul/ex2.bf16x2`；需要显式 mixed native/composite lowering。
- 将 MMA 支持显式移交给独立 active change `support-custom-mma`。

## Capabilities

### New Capabilities

- **Repository-local custom decode path**：对不存在于 Spike `encoding.h` 的 custom 指令，项目可在本地 decoder 中直接识别并进入 `--require-known` / PTX emit 流水线。
- **Non-MMA custom instruction support**：除 MMA 之外的 custom 指令可进入 decode + PTX emit + compile-first + microtest 验证路径。

### Modified Capabilities

- **Instruction support contract**：当前 `inst-support` 对 Spike whitelist 的口径需要补充范围说明，明确其覆盖“Spike-backed pattern inputs”，而不是“所有未来新增指令的唯一入口”。
- **PTX target baseline planning**：custom 指令支持所需的 PTX target / compile-first baseline 已先在 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中冻结为 shared active baseline，再由本 change 负责据此落地。

## Impact

- 现有 decode 架构需要从“Spike pattern + scalar fallback”扩展到“三路结构”：Spike-backed pattern、repo-local custom decode、scalar fallback。
- 非 MMA 指令虽然被拆成独立 change，但并不只是补 emitter case；它们仍要求先扩展 decode IR，并将 PTX target baseline 提升到 shared baseline 文档冻结的 `sm_89`。
- 本 change 的实现顺序会被 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中的 shared prerequisite 显式约束。
- MMA 风险被移交到独立 active change `support-custom-mma`；本 change 不再承诺 MMA shape/type 的任何支持范围。
- 一旦 baseline 决策完成并高于当前 `sm_75`，历史文档/命令示例与回归默认值都必须一起同步，不能只改 custom 专项路径。

## Documentation Impact

本 change 落地时至少需要检查并更新以下文档：

- `README.md`
- `doc/README.md`
- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`
- 如需调整 Spike whitelist 口径边界，补充检查 `openspec/specs/build-time-spike-pattern-subset/spec.md`

本 change 不 supersede 任何现有 active proposal；它与新增的 active change `support-custom-mma` 共同覆盖原先被打包讨论的 custom 指令主题，其中本 change 只承担 non-MMA 范围。
