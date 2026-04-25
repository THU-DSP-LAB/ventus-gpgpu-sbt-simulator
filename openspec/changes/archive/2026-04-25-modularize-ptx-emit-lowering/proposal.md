## Why

当前 `sbt/ptx_emit.cpp` 已经从“依赖 mnemonic string 做 lowering authority”向“依赖 decode/shared metadata 产出的结构化 descriptor/payload 做 authority”完成了一轮关键收敛，但 emitter 自身的组织形态仍明显滞后于这条新 contract。

现状问题不只是文件大，而是**宿主状态、ABI/helper 原语、以及各语义域 lowering** 仍混在同一个实现体内：

- `EmitCtx` 同时承担 module/function body 拼装、寄存器与 blob ABI 约定、地址映射 helper、prologue/epilogue，以及 control/scalar/vector/custom/mma 几乎全部 lowering；
- `emit_one_inst()` 仍是一条超长早返回分发链，主流程排错时必须在 control、scalar、vector、custom、mma 之间来回跳读；
- 当前分发顺序本身带有 correctness 含义，但这些 precedence 没有被显式固化成 domain-level contract，而是隐含在一个大函数的文本顺序里；
- 部分逻辑的真实归属点容易被误读，例如 builtin lowering 语义上看似属于 scalar/vector/math，但它们的 ownership 实际在 direct-call resolution 路径上；
- `mma` 虽然已有 `sbt/ptx_mma.*` 负责 planner/ABI 侧结构化信息，但真正发射 PTX 的 materialization 仍与其它 lowering 交织在 `ptx_emit.cpp` 内，导致“planner 已模块化，emit 仍集中耦合”的半拆状态；
- 现有验证资产和静态检查也默认假设 emitter 主要在单文件内，例如 `tools/check_ptx_emit_name_allowlist.py` 目前只扫描 `sbt/ptx_emit.cpp`，一旦开始模块化，验证口径若不同步就会退化。

因此，下一步最实际的收益不是“为控制行数拆文件”，也不是引入新的抽象层来包装当前逻辑，而是把 emitter 组织形态收敛到与当前 lowering authority 一致的模块边界上：

- **宿主 core 负责状态与共享 contract；**
- **lowering units 负责消费 descriptor/payload 并发射对应语义域的 PTX。**

本 change 的重点是提升主流程可维护性、降低跨语义域耦合、让测试和 review 能按 lowering domain 对齐，而不是追求“每个文件不超过多少行”的机械目标。

## What Changes

- 将 `sbt/ptx_emit.cpp` 的内部组织重构为“shared emitter core + 多个 lowering units”的形态，保留 `sbt/ptx_emit.hpp` 对外 API 不变。
- 明确 shared emitter core 的职责边界：只保留 module/function body 拼装、固定寄存器与 blob ABI contract、地址映射与 shared/global 数值地址 helper、virtual temp/label 分配、prologue/epilogue、CFG block 遍历与 fallthrough 发射，以及 instruction-dispatch 入口需要统一执行的 shared precondition 校验。
- 将 instruction-level lowering 从单一超长分发链中拆出，按**语义 authority** 而不是按文件行数组织为稳定单元，首选边界为：
  - `control`：structured control、ret、direct call、builtin call lowering、branch、CSR
  - `scalar`：scalar memory、scalar int、scalar fp
  - `vector`：vector memory、vector register、vector int、vector fp、compare、convert、mask
  - `custom`：custom non-MMA
  - `mma lowering`：MMA PTX materialization（继续复用现有 `sbt/ptx_mma.*` planner/ABI helper）
- 将 `emit_one_inst()` 收敛为薄分发器，显式固化 domain precedence，避免 correctness 继续依赖“大函数里的文本顺序”。
- 保持当前 descriptor/payload authority 不变，不在本 change 中引入新的 external API 或新的独立 IR 图结构。
- 保持当前 current lowering contract 不变；本 change 关注的是 emitter 内部模块化与解耦，不把它扩展成新的语义支持面或新的 fallback 行为。
- 更新验证与静态检查，使其覆盖模块化后的 emitter 文件集，而不是继续假设所有 authority-site 都集中在单一 `ptx_emit.cpp`。
- 将 `external_mnemonic_contract_test` 继续保留在最小验收面内，确保 comments / diagnostics / external symbol 等 external mnemonic contract 不会因为 emitter 多 translation units 化而退化。
- 将文档同步纳入 change，更新 current/active 口径，避免后续维护者继续把 emitter 视为单文件结构。

本 change 明确不做：

- 不为了“拆文件”而引入 registry/plugin 框架、虚函数层次或额外 runtime indirection；
- 不为了缩小单文件行数而把紧耦合 helper 生硬拆散；
- 不修改 `sbt/ptx_emit.hpp` 的 public API；
- 不改变 current descriptor/payload contract；
- 不引入 silent fallback、mock path 或新的“临时 guardrail”来掩盖模块化过程中的错误；
- 不把 `cfg.cpp` / `cfg_verify.cpp` 一并改写成新的模块架构；
- 不直接修改上级 `ventus-env` 子项目源码。

## Capabilities

### New Capabilities

- 新增 **ptx-emitter-lowering-modularity**：PTX emitter 内部实现可按 lowering semantic domain 维护、review 和验证，而不是依赖单一超长分发函数维持结构。
- 新增 **explicit-emitter-core-boundary**：shared emitter state/ABI/helper contract 与 domain-specific lowering 拥有清晰的实现边界，后续维护者可在不通读全部 lowering 分支的前提下定位主流程问题。

### Modified Capabilities

- 修改 **ptx-module-emission maintainability contract**：current emitter 的主流程组织从“单文件内嵌全部 lowering”调整为“shared core + domain units”，但 external behavior 与 public API 保持一致。
- 修改 **PTX emitter structural validation**：静态 allowlist / poison-name / compile-first / unit regression 等验证入口需要覆盖多 translation units，而不再默认只检查单个 `sbt/ptx_emit.cpp`。

## Impact

- 正向收益：
  - 主流程维护和 review 可以直接按 lowering domain 聚焦，不再需要在一个超长函数里追分支。
  - 共享 ABI/helper 与 domain lowering 的耦合边界更清楚，降低未来继续演进 scalar/vector/custom/mma 时的非局部影响。
  - `mma` 现有 planner/ABI 模块化成果能在 emit 层真正闭环，而不是继续半集中在 `ptx_emit.cpp` 中。
  - 测试与静态检查可以更准确地映射到 control/scalar/vector/custom/mma 等语义域。
- 主要风险与需要规避的错误：
  - 若拆分只按文件行数进行，会把真实共享 contract 切碎，反而扩大耦合面。
  - 若把 builtins 按“数学语义”而不是按“call ownership”迁移，容易造成 direct-call 路径和 builtin resolution 分裂。
  - 若过早引入新的 facade/registry，可能把当前清晰的 descriptor authority 稀释成另一套抽象债务。
  - 若静态检查与测试脚本不一起迁移，模块化后可能出现 authority regression 但工具没有覆盖到。
- 受影响区域：
  - `sbt/ptx_emit.*`
  - 新增的 emitter internal headers / lowering source files
  - `CMakeLists.txt`
  - `tools/check_ptx_emit_name_allowlist.py`
  - 相关 emitter regression tests
  - `tools/README.md`
  - `README.md`
  - `doc/IMPLEMENTATION_CODEMAP.md`
  - `doc/README.md`
  - `openspec/README.md`
- 预期结果：
  - current PTX emission behavior、descriptor authority、ABI contract 与 public API 保持不变；
  - emitter 实现改为模块化、可按语义域维护；
  - 主流程 review 不再依赖在一个 3000+ 行文件里来回跳找分支；
  - 验证工具能够对新的模块化边界保持同等或更强的约束。

### Documentation Impact

- `README.md`
- `doc/README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `tools/README.md`

### Superseded Notes

- 本 change 吸收并 supersede 当前“仅讨论 `ptx_emit.cpp` 是否应该按 control/scalar/vector/custom/mma 拆分”的零散讨论；后续若继续推进 emitter 结构收敛，应以本 change 的 proposal/design/tasks 为 active 入口，而不是继续在会话或临时笔记中维护并行方案。
