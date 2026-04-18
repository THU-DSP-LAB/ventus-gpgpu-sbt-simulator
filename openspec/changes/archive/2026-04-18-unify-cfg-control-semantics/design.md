## Context

本设计描述的是本 change 的 **landed behavior 与设计边界**。

当前仓库已经具备一套足以表达 main-pipeline 控制流语义的结构化基础：

- `sbt/instruction_metadata.cpp` 已为 `beq/vb*/jal/jalr` 产出 `EmitDescriptor::control_kind` 与 `branch_cond`；
- 同一层已为 `setrpc/join/barrier/vsetvli/endprg` 产出 `EmitDescriptor::structured_control_kind`；
- `finalize_emit_descriptor()` 已把 `jal` 细化为 `DirectJump/DirectCall`，把 `jalr` 细化为 `Return/IndirectTerminator`；
- `sbt/ptx_emit.cpp` 已经在 current supported path 上消费这些结构化控制流语义。

本 change 落地前，其它控制流消费者还没有对齐：

- `sbt/cfg.cpp` 仍自行按 `di.name` 分类 terminator / edge；
- `sbt/cfg_verify.cpp` 仍自行按 `di.name` 分类 `setrpc/vbranch/join/barrier/jalr`，向前回溯 `auipc` 时也仍看 mnemonic；
- `tools/sbt_ptx.cpp` 在 direct-call 闭包扫描里仍用 `di.name == "jal"` 识别 direct call。

因此当前主流程虽然“能跑”，但 authority 仍然分裂：

- decode 已产出最终控制流语义；
- emit 已消费该语义；
- cfg / verify / call-graph scan 仍在 consumer 侧重建 mnemonic 语义。

本设计依赖以下 current canonical docs / specs：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`

## Goals / Non-Goals

**Goals:**

- 让 decode 产出的结构化控制流语义成为 `cfg`、`cfg_verify`、`tools/sbt_ptx.cpp`、`ptx_emit` 的共同 authority。
- 删除 `cfg` / `cfg_verify` / direct-call 闭包扫描中的 supported-path `di.name` 控制流分派。
- 通过共享 helper 收口控制流分类逻辑，避免多个消费者各自维护 `jal/jalr/vb*/join/endprg/barrier` 字符串规则。
- 对缺失或不一致的控制流语义建立显式 fail-fast 边界。
- 在实现落地后，把“CFG / verify 仍 deferred”的旧口径从 current 文档中移除，并同步更新相关导航。

**Non-Goals:**

- 不重做 `DecodedInst` 结构，也不强制把 `EmitDescriptor` 立即改名为全局语义对象。
- 不移除 decode/shared metadata lookup 对 mnemonic name 的内部 key 依赖。
- 不把本 change 扩展成 ordinary 非控制流 lowering 的再次重构。
- 不把本 change 扩展成 `replicated-scalar-state` 主合同的改写；这次只收口 control semantics authority，而不是改写 scalar-state execution model。
- 不修改上级 `ventus-env` 子项目。

## Decisions

### 1. Reuse the existing decode-produced control semantics instead of introducing a new IR

本 change 不新增独立控制流 IR，而是直接复用当前 decode 已经写入 `DecodedInst.emit` 的结构化语义：

- `EmitDomain::Control`
- `ControlKind`
- `BranchCondKind`
- `EmitDomain::StructuredControl`
- `StructuredControlKind`

必要时配合现有 ordinary metadata，例如 `ScalarIntKind::Auipc`。

这样做的原因是：

- 当前 `ptx_emit` 已经证明这些字段足以表达 supported-path 控制流语义；
- 问题不在“缺语义”，而在“下游消费者还没统一消费它”；
- 先收口 authority，比先发明新对象更低风险，也更符合当前仓库状态。

**Alternatives considered:**

- 新增独立 `ControlSemantics` IR 挂到 `DecodedInst`：
  - 拒绝，因为会与现有 `EmitDescriptor` 形成重复 authority，收益不足以覆盖迁移成本。
- 继续让每个消费者维护本地 mnemonic helper：
  - 拒绝，因为这正是当前 authority split 的根源。

### 2. Introduce a shared control-semantics helper as the only consumer-side classifier

本 change 新增一个共享 helper 模块（`sbt/control_semantics.{hpp,cpp}`），专门为控制流消费者提供统一分类入口。

该 helper 的职责是：

- 从 `DecodedInst` 读取结构化控制流语义；
- 产出 `cfg` / `cfg_verify` / `tools/sbt_ptx.cpp` 所需的统一分类结果；
- 对结构化字段缺失、类型不匹配、或与 supported contract 自相矛盾的情况显式报错；
- 统一负责 branch / jump / direct-call 的 `inst_pc + imm` target 计算，避免消费者再次抄写 bundle-pc / inst-pc 规则；
- 统一负责 `setrpc` + `ScalarIntKind::Auipc` 的 join 解析入口，避免 `cfg_verify` 在 consumer 侧继续保留 mnemonic 回溯。

该 helper 不重新存一份语义，只做：

- authority read
- consumer-friendly classification
- fail-fast validation
- shared target / PC resolution

这样可以避免：

- `cfg.cpp` 与 `cfg_verify.cpp` 各自维护一套 `is_vbranch/is_ret/is_indirect_jalr` helper；
- `tools/sbt_ptx.cpp` 再维护第三套 direct-call 判定。

**Alternatives considered:**

- 让各个消费者直接散读 `di.emit.*`：
  - 拒绝，因为这会把访问路径再次复制到多处，未来仍容易漂移。
- 把 helper 塞进 `cfg.cpp` 或 `cfg_verify.cpp` 私有实现：
  - 拒绝，因为这样无法成为 main-pipeline 共享 authority 入口。

### 3. CFG and CFG verify both become descriptor-first for supported control-flow decisions

`sbt/cfg.cpp` 的 target behavior 是：

- `join` leader 判定来自 `StructuredControlKind::Join`
- `endprg` no-succ terminator 判定来自 `StructuredControlKind::EndPrg`
- branch / jump / call / return / indirect terminator 判定来自 `ControlKind`
- branch 条件种类来自 `BranchCondKind`

`sbt/cfg_verify.cpp` 的 target behavior 是：

- `setrpc` 来自 `StructuredControlKind::SetRpc`
- `vbranch` 来自 `ControlKind::VectorBranch`
- `join` / `barrier` 来自 `StructuredControlKind::Join/Barrier`
- unsupported `jalr` 来自 `ControlKind::IndirectTerminator`
- `auipc` 回溯基址识别来自 `ScalarIntKind::Auipc`

这意味着 supported-path control analysis 不再允许把 mnemonic string 当作 correctness authority。

**Alternatives considered:**

- 只改 `cfg.cpp`，保留 `cfg_verify.cpp` 的名字分类：
  - 拒绝，因为 verify 仍会成为剩余 authority split。
- 只改 `cfg_verify.cpp`，保留 `cfg.cpp` 的名字分类：
  - 拒绝，因为 CFG block/edge 本身就是 verify 的输入，源头不收口意义有限。

### 4. Direct-call closure scan is part of the same authority boundary

`tools/sbt_ptx.cpp` 里的 direct-call 闭包扫描虽然不在 `sbt/cfg.*` 内，但它属于 main pipeline 的控制流消费者，因此也必须纳入同一条 authority 规则。

target behavior 是：

- direct-call 识别来自 `ControlKind::DirectCall`
- direct-call target 计算继续使用现有 `inst_pc + imm`
- builtin callee 过滤继续按现有 symbol contract 工作
- prototype / `.func` 闭包路径继续通过 `ptx_emit_call_prototype_test` 与主流程 smoke 一起回归，避免 authority 收口破坏 helper 前向声明路径

如果保留这个点的 `di.name == "jal"` 判定，那么主流程外围仍然有一处 mnemonic authority，没有真正完成收口。

**Alternatives considered:**

- 把它视为 tool-local 细节，留待以后再改：
  - 拒绝，因为它直接影响 call graph closure，属于主流程 correctness，不是纯工具输出。

### 5. Fail-fast replaces silent mnemonic fallback for supported control-flow paths

本 change 延续仓库的 debug-first / fail-fast 原则：

- supported-path control-flow consumer 缺结构化语义时，必须显式失败；
- 不允许“因为当前字段不完整，所以回退到 `di.name`”；
- poison-name regression 应当成为验证 contract 的一部分，而不是附加锦上添花。

这条规则覆盖：

- CFG build
- CFG verify
- direct-call closure scan

不覆盖 external mnemonic contract；`DecodedInst.name` 仍可用于 pretty / JSON / diagnostics / reporting。

**Alternatives considered:**

- 临时保留 name fallback 作为兼容路径：
  - 拒绝，因为这会再次引入 silent split，违背本 change 的目标。

## Risks / Trade-offs

- `EmitDescriptor` 这个名字本身偏 emitter；把它作为 CFG/verify 的 authority 在命名上不够理想。但当前字段语义本身已经是 backend-neutral，短期这是可以接受的命名债。
- 共享 helper 会把原本分散的控制流判断集中起来，短期需要更认真地设计分类边界；但这正是消除漂移的必要成本。
- poison-name regression 可能暴露出当前手工构造测试 fixture 里“只改 name，没改 descriptor”的旧假设；这是预期内的收敛成本。
- regext-bundled control-flow 指令同时依赖 `bundle pc` 与 `inst_pc` 语义；若回归覆盖不够，这次 authority 收口容易意外破坏现有 PC 约定。
- `EmitDescriptor` 这个名字本身仍偏 emitter；虽然字段当前已同时服务 CFG / verify / call-graph scan，但本 change 不扩成一次更大的 descriptor 重命名或 IR 重构。
