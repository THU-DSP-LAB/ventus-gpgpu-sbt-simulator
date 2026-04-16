## Why

当前仓库已经把 PTX 标量主线收敛到 replicated active-lane scalar state，但关键指令语义仍分散在多处字符串判断中：

- decode 端仍通过 mnemonic 与后缀推断 operand form / immediate form；
- CFG verify 的 uniform 传播仍通过 `_vx/_vi/_vv/_v` 等后缀做二次推断；
- PTX emitter 的 scalar 执行分类仍依赖名字表，且未显式列出的 scalar 指令会默认落到 `UniformPure`。

这类实现对“完全 unsupported”的输入仍然能 fail-fast，但对“能解码、能 emit、却把 lane / leader / fixed-lane 语义猜错”的输入保护不足。随着后续继续扩展 Ventus ISA，这会增加以下风险：

- decode、verify、emit 对同一条指令出现不同步的语义解释；
- 新增 scalar 指令时，因为默认 `UniformPure` 而静默放行 all-lane lowering；
- 新增向量指令形态时，只改一处 suffix 规则，另一处仍沿用旧推断。

本 change 的目标不是全面重写 emitter dispatch，而是先把最危险的“隐式推断 + 默认放行”收敛为显式 contract。

## What Changes

- 为 Spike-pattern-backed 常规指令引入共享的 repository-managed 指令元数据，至少统一：
  - operand form
  - immediate form
  - vector uniform-transfer behavior（供 CFG verify 使用）
- decode 不再把 mnemonic 后缀作为主要事实源，而是通过共享元数据填充 `DecodedInst` 的 operand / immediate 结构字段。
- CFG verify 不再重复维护 `_vx/_vi/_vv/_v` 风格的 suffix 推断，而是消费共享元数据完成 uniform 传播。
- PTX emitter 的 scalar 执行分类改为显式枚举、显式分类、默认报错：
  - 当前已支持 scalar 指令必须逐条声明其 `uniform-pure` / `lane-sensitive` / `fixed-lane-sensitive` / `externally-side-effecting` 语义；
  - 未显式分类的 scalar 指令不得默认落到 `UniformPure`。
- 同步更新 current contract 与仓库导航文档，确保 `README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`openspec/README.md` 对该 change 的口径一致。

本 change 明确不做：

- 不把整个 PTX emitter 的所有 lowering dispatch 全部改写成 `enum opcode -> switch`；
- 不新增独立的 semantic IR 层；
- 不顺带扩展新的 Ventus ISA 支持面，只收敛现有 current supported subset 的元数据与 fail-fast 约束。

## Capabilities

### New Capabilities

- 新增“共享指令元数据”能力：decode、CFG verify、PTX emitter 可以复用同一份 repository-managed instruction metadata，而不是分别按字符串后缀推断。
- 新增“未分类 scalar 默认拒绝”能力：当新指令没有显式 execution-semantics 分类时，PTX emitter 必须报错而不是默认按 `uniform-pure` 放行。

### Modified Capabilities

- 修改 `inst-support` current contract：Spike-pattern-backed 常规指令的 operand / immediate / verify metadata 必须由共享元数据描述，而不是由多个阶段独立 suffix 猜测。
- 修改 `replicated-scalar-state` current contract：scalar execution classification 必须是显式、完整、默认拒绝未分类项的 contract，而不是“名字表 + 默认 `UniformPure`”。

## Impact

- Affected areas:
  - `sbt/riscv_decode.*`
  - `sbt/cfg_verify.*`
  - `sbt/ptx_emit.*`
  - 与上述 contract 对应的测试
- Expected outcome:
  - 当前支持子集在行为上保持不变，但新增指令时更难出现“翻译成功、语义静默漂移”的情况；
  - decode / verify / emit 三处对同一条指令的语义来源收敛到单点维护。

### Documentation Impact

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`
