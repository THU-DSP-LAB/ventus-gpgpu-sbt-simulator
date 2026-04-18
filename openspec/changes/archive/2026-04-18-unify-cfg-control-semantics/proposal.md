## Why

上一个 lowering authority 收敛阶段已经把 `sbt/ptx_emit.cpp` 的 current supported correctness path 切到 `DecodedInst.emit` / `DecodedInst.custom` / `DecodedInst.mma`，其中控制流部分也已经消费 `EmitDescriptor::control_kind`、`structured_control_kind` 与 `branch_cond`。

但当前主流程的控制流消费者仍然没有完全跟上这条 contract：

- `sbt/cfg.cpp` 仍用 `di.name` 区分 `beq/vb*/jal/jalr/endprg/join`，自己维护 terminator / edge 分类；
- `sbt/cfg_verify.cpp` 仍用 `di.name` 区分 `setrpc/vbranch/join/barrier/jalr`，并且向前回溯 `auipc` 时仍看 mnemonic；
- `tools/sbt_ptx.cpp` 在 direct-call 闭包扫描里仍用 `di.name == "jal"` 识别 callee。

这使当前流水线出现了明显的 authority split：

- decode 已经产出控制流最终语义；
- emit 已经消费这份语义；
- cfg / cfg_verify / 主流程外围 call-graph 扫描却还在重新解释 mnemonic。

结果是：

- 同一条指令的控制流语义存在多处事实源；
- 后续若调整 control descriptor 或 decode 细化逻辑，仍可能出现“emit 改了，CFG/verify 没改”的漂移；
- `DecodedInst.name` 仍在主流程 correctness path 的一部分承担 semantic authority，而不仅是 external mnemonic contract。

本 change 的目标就是把这块 deferred work 正式落地：让 decode 产出的结构化控制流语义成为 `cfg`、`cfg_verify`、`tools/sbt_ptx.cpp` 的共同 authority。

## What Changes

- 把 current control-flow contract 明确为：
  - decode / shared metadata 负责产出控制流最终语义；
  - `cfg`、`cfg_verify`、`tools/sbt_ptx.cpp` 与 `ptx_emit` 都只消费这份结构化语义；
  - `DecodedInst.name` 继续保留为 pretty / JSON / diagnostics / reporting 的 external mnemonic contract，但不再作为上述控制流消费者的 correctness authority。
- 为控制流消费者引入共享的 helper / classifier，统一读取以下已存在的结构化字段：
  - `EmitDescriptor::domain`
  - `EmitDescriptor::control_kind`
  - `EmitDescriptor::structured_control_kind`
  - `EmitDescriptor::branch_cond`
  - 必要时配合 `ScalarIntKind::Auipc` 这类已存在的 ordinary metadata
- 将 `sbt/cfg.cpp` 的 leader / terminator / edge 分类切到结构化控制流语义，覆盖至少：
  - scalar branch
  - vector branch
  - direct jump
  - direct call
  - return
  - indirect terminator
  - `join`
  - `endprg`
- 将 `sbt/cfg_verify.cpp` 的结构化校验切到同一份语义，覆盖至少：
  - `setrpc`
  - `vbranch`
  - `join`
  - `barrier`
  - `jalr` 非 `ret` 形态
  - `auipc` 回溯识别
- 将 `tools/sbt_ptx.cpp` 的 direct-call 闭包扫描切到 `ControlKind::DirectCall`，消除主流程外围对 `jal` mnemonic 的直接依赖。
- 为 supported-path 控制流语义缺失或自相矛盾的情况建立显式 fail-fast 行为，而不是退回 `name` 判定。
- 补充回归，验证：
  - 改坏 `DecodedInst.name` 不会改变 supported-path CFG / verify / callee-scan 控制流语义；
  - 结构化字段缺失时会显式报错；
  - existing current behavior（block splitting / edge kind / verify result / unsupported jalr reporting）保持不变；
  - regext-bundled control-flow instruction 的 `bundle pc` / `inst_pc` 语义保持不变。
- 在实现落地后同步更新 current 文档与 OpenSpec 导航，把“CFG / cfg_verify 仍是 deferred”的旧口径替换为新的 as-built 真相；在本 change 仍处于 `active` 阶段时，不提前改写 current 文档事实。

本 change 明确不做：

- 不重写 `DecodedInst` 结构，也不强制把 `EmitDescriptor` 立即重命名为全局语义对象；
- 不把 decode 内部 metadata lookup 的 mnemonic key 去掉；
- 不扩大到 ordinary 非控制流 lowering 的再次重构；
- 不修改上级 `ventus-env` 子项目源码。

## Capabilities

### New Capabilities

- 新增“shared control semantics for CFG consumers”能力：`cfg`、`cfg_verify`、`tools/sbt_ptx.cpp` 与 `ptx_emit` 共享 decode 产出的结构化控制流语义，而不是各自维护一套 mnemonic 解释。
- 新增“control-semantics poison-name regression”能力：仓库可以显式验证 external mnemonic contract 与主流程 control-flow authority 已经解耦。

### Modified Capabilities

- 修改 `inst-support` current contract：当前 supported-path 的控制流语义 authority 不仅对 PTX emitter 生效，也对 CFG build / CFG verify / direct-call 闭包扫描生效。

## Impact

- Affected areas:
  - `sbt/cfg.*`
  - `sbt/cfg_verify.*`
  - `sbt/instruction_metadata.*`
  - `sbt/riscv_decode.*`
  - `tools/sbt_ptx.cpp`
  - 相关 contract / regression tests
- Expected outcome:
  - `decode -> cfg -> cfg_verify -> emit -> call-graph scan` 的控制流语义 authority 收敛到单点；
  - 后续调整 `ControlKind` / `StructuredControlKind` / `branch_cond` 时，不再需要同步维护多处 `di.name` 判断；
  - `DecodedInst.name` 在主流程中进一步收缩为 external mnemonic contract，而不是 correctness authority；
  - 在实现完成后，current 文档不再继续把 CFG / verify 的 name-string 依赖描述为 as-built deferred 状态。

### Documentation Impact

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`

### Superseded Notes

- 本 change 承接 `reduce-lowering-name-dependence` 已经明确标注的 CFG / `cfg_verify` deferred control-flow work；相关 historical 讨论继续保留在 archive 中，但不再作为当前 active 入口。
