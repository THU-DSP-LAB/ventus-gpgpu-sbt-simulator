## Context

本设计描述的是 **target behavior**：把当前 decode / CFG verify / PTX emitter 中分散的字符串推断收敛为共享 instruction metadata contract，同时保留当前 current supported subset 的外部行为不变。

本设计依赖以下 current canonical docs / specs：

- `openspec/specs/inst-support/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`
- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`

当前源码中的主要问题点是：

- `sbt/riscv_decode.cpp` 对 Spike-backed 指令仍通过 mnemonic 后缀推断 operand / immediate 形态；
- `sbt/cfg_verify.cpp` 对 uniform transfer 仍维护一套独立 suffix 规则；
- `sbt/ptx_emit.cpp` 的 scalar execution classification 仍是名字表，且未显式列出的 scalar 会默认落到 `UniformPure`。

这三处不是三个独立问题，而是同一个根因：缺少共享、结构化、默认拒绝的 instruction metadata contract。

## Goals / Non-Goals

**Goals:**

- 用一份 repository-managed instruction metadata 作为 decode 与 CFG verify 的共享事实源。
- 让 PTX emitter 的 scalar execution classification 变成显式、完整、默认拒绝未分类项的 contract。
- 保持当前 current supported subset 的可观察行为不变，只收敛维护方式与 fail-fast 边界。
- 把文档与 spec 的 current 口径同步到新的 metadata contract 上。

**Non-Goals:**

- 不在本 change 中引入新的 semantic IR 层。
- 不把整个 PTX emitter 的 lowering dispatch 全量改写成 `switch(inst_id)`。
- 不扩展新的 Ventus ISA 支持面。
- 不修改 custom non-MMA / MMA 已有的 `CustomInstInfo` / `MmaInstInfo` contract，只要求它们继续遵守“显式 metadata、默认拒绝不完整 contract”的原则。

## Decisions

### 1. Introduce a minimal shared `InstId + InstMetadata` contract

为当前 supported subset 引入最小共享 contract：

- `InstId`
  - 稳定标识一条仓库支持的指令语义身份；
  - `DecodedInst` 挂载该 id；
  - `name` 保留给 pretty print、JSON 输出、诊断与现有测试断言使用，但不再作为主要语义来源。
- `InstMetadata`
  - 至少覆盖：
    - `operand_form`
    - `imm_kind`
    - `uniform_transfer_kind`
    - 对 scalar 指令可选挂载 `scalar_exec_kind`

目标不是一次性把所有 lowering 语义都塞进 metadata，而是先把当前最容易漂移的“decode 形态 + CFG uniform 传播 + scalar execution classification”收敛到单点。

**Alternatives considered:**

- 继续保留 `name + suffix` 规则，只是把 helper 函数放到一个文件里：
  - 拒绝，因为这只能减少重复代码，不能消除“未分类默认猜测”的风险。
- 直接引入完整 semantic IR：
  - 拒绝，因为改动面过大，不适合当前这个以风险收敛为目标的 change。

### 2. Decode becomes metadata-populating, not suffix-inferring

decode 的目标行为调整为：

- scalar decode 路径在识别指令时直接写入 `InstId`；
- Spike-backed pattern 路径在 pattern 命中后，用 repository-managed metadata 查出 `InstId` 与对应的 `operand_form / imm_kind`；
- repository-local custom paths 继续使用各自显式 metadata，但 `DecodedInst` 仍需满足统一的结构字段约束。

这意味着：

- `sbt/riscv_decode.cpp` 不再维护一套按 `_vx/_vi/_vv/_v` 猜字段的主逻辑；
- irregular mnemonic 不需要为了迁就 suffix 规则而在多个阶段打补丁；
- `DecodedInst` 的 operand / immediate 字段由 metadata 明确填充。

**Alternatives considered:**

- 保留 decode suffix 规则，只让 CFG verify 共享它：
  - 拒绝，因为 decode 本身就是规则漂移源头之一，保留它等于保留双事实源。

### 3. CFG uniform propagation consumes explicit metadata

`sbt/cfg_verify.cpp` 的 uniform transfer 判断改为消费 `InstMetadata.uniform_transfer_kind`，而不是重新解析 mnemonic 后缀。

最小需要覆盖的 transfer 行为包括：

- `always-uniform-dst`
- `never-uniform-dst`
- `uniform-if-rs1`
- `uniform-if-rs2`
- `uniform-if-rs1-and-rs2`

若某条指令需要更特殊的传播规则，可以在 metadata 中新增显式 kind，而不是在 verify 里追加新的字符串例外。

**Alternatives considered:**

- 继续在 verify 中维护 suffix 规则，并要求 decode 测试兜住：
  - 拒绝，因为 decode 正确不等于 verify 语义正确，两个阶段仍会各自漂移。

### 4. Scalar execution classification becomes exhaustive and fail-fast

`ScalarExecKind` 继续沿用 current contract 中的四类：

- `uniform-pure`
- `lane-sensitive`
- `fixed-lane-sensitive`
- `externally-side-effecting`

但实现约束改为：

- 对当前 supported scalar subset，必须逐条显式分类；
- 分类查询失败时直接报错；
- 不允许把“查不到”解释成 `UniformPure`。

这条设计只改变 guard / contract，不强制要求 emitter 立刻把所有 lowering site 都改写成 `switch(inst_id)`。现有按指令名分派具体 PTX lowering 的代码可以暂时保留，但：

- 决定 all-lane / leader-only / fixed-lane 行为的 guard 必须来自显式分类；
- 新增 scalar 指令时，先补分类，再谈 lowering。

**Alternatives considered:**

- 保留默认 `UniformPure`，靠测试覆盖发现问题：
  - 拒绝，因为这会把 contract 缺口变成 silent fallback，不符合 current fail-fast 原则。

### 5. Scope the first migration to the high-risk metadata only

本 change 的第一阶段收敛范围限制为：

- Spike-backed non-custom instructions 的 decode / CFG-analysis metadata
- current supported scalar subset 的 execution-semantics metadata

明确后置但不在本 change 中处理的内容：

- emitter 全量 lowering dispatch 从 `di.name` 迁移到 `InstId`
- 更高层 semantic IR
- 对 custom non-MMA / MMA metadata 的进一步统一抽象

这样可以先把“最危险的 silent semantic drift”收住，同时避免把 change 扩成大规模重构。

**Alternatives considered:**

- 一次性把所有非 custom / custom / MMA 全部纳入同一 mega-metadata：
  - 拒绝，因为会显著放大实现与回归面，不利于快速建立默认拒绝边界。

## Risks / Trade-offs

- 引入 `InstId + InstMetadata` 会增加一层表驱动维护成本，但这是有意的显式成本，用来替代当前的隐式漂移成本。
- 现有测试里有不少直接断言 mnemonic 文本；保留 `DecodedInst.name` 可以减少迁移扰动，但也意味着短期内代码里仍会存在一部分字符串 dispatch。
- 默认拒绝未分类 scalar 后，短期内可能暴露之前被默认 `UniformPure` 掩盖的路径；这是预期收益，不是回归。
- `openspec/README.md` 当前 active change 列表与目录真实状态不一致，本 change 需要把这类导航一致性修掉，但不应扩展成一次性文档清理项目。
