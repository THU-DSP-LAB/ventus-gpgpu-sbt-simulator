## Context

> Status: `active target design`
>
> 本文描述 `modularize-ptx-emit-lowering` change 的目标设计，不表示当前仓库已经完成该结构收敛；当前实现真相仍以 `sbt/ptx_emit.cpp`、`doc/IMPLEMENTATION_CODEMAP.md` 和 current specs 为准。

当前 emitter 已经具备几个重要前提：

- current supported PTX emission path 已转成 descriptor/payload-authoritative，而不是继续以 mnemonic string 作为 correctness authority；
- `%tmp*` virtual temp、replicated scalar state、direct-call value ABI、current MMA planner/ABI helper 都已是 current contract 的一部分；
- `mma` 的 planner/ABI 结构已经被抽到 `sbt/ptx_mma.*`，说明仓库接受“共享 contract + 语义子模块”的实现方式；
- 现有 emitter regression 已按 call ABI、leader-lane、custom、mma 等语义面分开维护。

但 `sbt/ptx_emit.cpp` 当前仍把三类职责放在同一实现体内：

1. shared emitter host
   - `EmitCtx` 状态
   - module/function body 拼装
   - fixed register / blob ABI / address mapping / prologue/epilogue
2. instruction-dispatch control
   - `emit_one_inst()` 主分发链
   - domain precedence
3. domain-specific lowering
   - control
   - scalar
   - vector
   - custom non-MMA
   - MMA PTX materialization

这导致当前代码的核心问题并非“一个文件 3k+ 行”本身，而是：

- 主流程 review 需要在一个混合函数里来回跳过多个语义域；
- `EmitCtx` 的 shared contract 与 lowering logic 没有实现级边界；
- dispatcher precedence 隐含在一个大函数的文本顺序里，不够显式；
- builtin lowering 容易被误认为应按“数学语义”归到 scalar/vector，但它的实际 ownership 在 direct-call resolution 上；
- `mma` 虽已有 planner/ABI 模块化成果，但 emit materialization 仍和其它 lowering 混在一起；
- 现有静态检查和结构性验证默认扫描单一 `sbt/ptx_emit.cpp`，模块化后若不同步迁移，验证强度会下降。

本设计依赖以下 canonical docs/specs：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`
- `openspec/specs/ptx-temp-register-allocation/spec.md`
- `openspec/changes/modularize-ptx-emit-lowering/specs/ptx-lowering-modularity/spec.md`

## Goals / Non-Goals

**Goals:**

- 将 emitter 的 shared host 与 domain-specific lowering 明确分层。
- 将 instruction dispatch 收敛为显式、可 review 的薄分发器。
- 让模块边界按 lowering authority 和共享 contract 划分，而不是按行数或“差不多均分”划分。
- 保持 current PTX emission behavior、descriptor authority、public API 与 ABI contract 不变。
- 把模块化后的静态检查、compile-first 和相关回归同步迁移，避免验证退化。
- 把 `mma` 在 emit 层真正闭环成独立 lowering domain，而不是继续处于“planner 已分离、emit 未分离”的半拆状态。

**Non-Goals:**

- 不引入新的 public emitter API。
- 不引入独立于 `DecodedInst`/current descriptors 之外的新 IR 图结构。
- 不把 `cfg.cpp` / `cfg_verify.cpp` 一并改写为同样的模块架构。
- 不为了拆分而引入 registry、plugin、虚函数树或额外 runtime dispatch。
- 不把 builtins 人为迁入 scalar/vector 域，只因为其最终 PTX 形态看起来像算术 lowering。
- 不把“减少单文件行数”本身当作成功标准。

## Decisions

### 1. Keep one shared `EmitCtx` host and move domain lowering out around it

本 change 不重新设计 emitter 宿主抽象，而是保留一个 shared internal host（继续使用 `EmitCtx` 这一角色），再把 domain-specific lowering 从宿主中迁出。

shared host 的职责保持集中：

- 持有 current emitter state
- 提供 `emit_line`、temp/label allocator、shared helper 原语
- 维护 module/function body assembly
- 维护 blob ABI、固定寄存器、prologue/epilogue、fallthrough emission

domain units 通过 shared host 暴露的 lowering primitives 工作，而不是各自维护 body/output state。

这样做的原因是：

- 当前最大的耦合并不在“状态对象过大”，而在“状态对象和 lowering 本体没有边界”；
- 若在模块化同时引入全新的 context facade，会把一次结构收敛扩大为两次抽象迁移；
- 当前 helper、ABI、blob/layout contract 都已经围绕 `EmitCtx` 成形，最稳妥的方式是固化宿主、迁出 lowering。

**Alternatives considered:**

- 重新设计一个全新的 lowering context/facade：Rejected，因为这会把一次模块化收敛扩大成抽象层重写，风险高于收益。
- 保持单文件，只把若干 helper 提到匿名 namespace：Rejected，因为无法解决 mixed-domain dispatch 和 ownership 不清的问题。

### 2. Split lowering by semantic authority, not by equal file size

模块边界按 semantic authority 划分，首阶段采用这组 domain：

- `core`
  - shared host
  - module/function assembly
  - fixed register and blob ABI contract
  - temp/label allocation
  - address mapping helpers
  - prologue/epilogue
  - shared precondition validation
  - CFG block traversal / fallthrough emission
- `control`
  - structured control
  - ret
  - direct call
  - builtin call lowering
  - branch
  - CSR
- `scalar`
  - scalar memory
  - scalar integer
  - scalar floating-point
- `vector`
  - vector memory
  - vector register
  - vector integer
  - vector floating-point
  - compare / convert / mask
- `custom`
  - custom non-MMA
- `mma lowering`
  - MMA PTX materialization
  - 继续依赖 `sbt/ptx_mma.*` planner / ABI helpers

这组边界不是唯一理论最优，而是当前代码现状下最稳妥的第一阶段边界：

- `scalar int / scalar fp / scalar memory` 共享 `x`-reg、Zfinx、classification、leader/all-lane 约定，第一步不应再细拆；
- `vector` 虽然很长，但其内部共享 `%v/%f/%p` 使用模式和若干 helper，先整体迁出即可获得主要收益；
- `custom non-MMA` 与 `mma` 拥有独立测试资产和语义 authority，应分开，而不是合并成“大 custom 文件”；
- `mma` 的 planning 已经在 `sbt/ptx_mma.*` 模块中，emit materialization 单独成域可以和 current repository shape 对齐。

**Alternatives considered:**

- 先按更细粒度拆成 `vector-int/vector-fp/vector-cmp/vector-convert` 等多个单元：Rejected，因为当前共享 helper 和 review 边界还没有天然细到这个粒度，过早细拆只会增加跨文件跳转。
- 只拆 `control/scalar/vector/custom`，把 MMA 留在 core：Rejected，因为这会保留当前最不自然的“planner 已独立、emit 未独立”半拆状态。

### 3. Builtin lowering stays in the control/call domain

虽然某些 builtin 最终会发射 scalar/vector/math-like PTX 序列，但它们的 ownership 不按“最后生成了什么指令”决定，而按“谁决定了这次 direct call 的语义去向”决定。

因此 builtin lowering 归 `control` 域，而不是归 `scalar` 或 `vector` 域。`control` 域负责：

- direct callee resolution
- direct `.func` call emission
- builtin-inline dispatch

这样可以避免出现：

- direct call 解析在 `control`
- builtin 名字分派在另一个域
- 两边共同维护 call-site 前置条件和 return-value 约定

这条决策是对前一版方案的一个明确修正：按“最终 PTX 看起来像什么”分配 ownership 会制造新的交叉耦合。

**Alternatives considered:**

- 把 builtin 按其发射结果迁去 scalar/vector：Rejected，因为 ownership 会和 direct-call resolution 分裂，反而扩大耦合面。

### 4. `emit_one_inst()` becomes a thin dispatcher with explicit precedence

instruction-level emission entrypoint 收敛为薄 dispatcher，形态类似：

```cpp
void EmitCtx::emit_one_inst(const BundleInst& bi) {
  emit_comment_if_needed(...);
  validate_shared_preconditions(...);

  if (try_emit_control(*this, bi)) return;
  if (try_emit_scalar(*this, bi.inst)) return;
  if (try_emit_vector(*this, bi.inst)) return;
  if (try_emit_mma(*this, bi.inst)) return;
  if (try_emit_custom(*this, bi.inst)) return;

  throw EmitError(...);
}
```

这里的关键不是具体函数名，而是：

- domain precedence 明确可见；
- supported-path correctness 不再依赖一个 mixed-domain 巨大分支链；
- 新增 supported lowering 时，维护者先决定归属域，再进入对应单元。

这里将 `mma` 放在 `custom` 之前，是为了显式保留当前“`custom.family == Mma` 进入 MMA lowering”的 ownership，而不是让 generic custom domain 先吃掉这部分路径。

这条 dispatcher 只做：

- 通用 comment / shared precondition
- domain order dispatch
- 最终 unsupported fail-fast

不再承载各域自身的 lowering 细节。

这里的 `shared precondition` 不是随意留给各 domain 自行复制的一组零散检查，而是 dispatcher/core 统一拥有的前置合同。至少包括：

- `include_comments` 下的 comment emission
- scalar execution classification metadata 的 central gate
- 任何当前 supported-path 必须在 domain dispatch 之前统一失败的共享前置条件

domain units 可以依赖这些前置条件已经成立，但不应各自再复制一份“先做 shared gate 再进入本域 lowering”的分散实现。

**Alternatives considered:**

- 继续使用一个大号 `if/else` 链，只把部分实现挪到 helper：Rejected，因为 precedence 仍被埋在 mixed-domain 分支文本中，主流程可读性收益有限。
- 引入 map/registry 式 handler 查找：Rejected，因为当前 domain 数量小且 precedence 有语义，registry 只会掩盖顺序和 ownership。

### 5. Core primitives stay centralized; domain units must not clone helper contracts

模块化后，domain units 可以调用 shared host primitives，但不得复制这些 contract：

- fixed register ownership
- blob ABI read/write
- shared/global address mapping
- leader selection / warp sync / replicated scalar helpers
- temp/register allocation

换句话说，domain units 的职责是“组合已有 contract 产出 domain lowering”，不是“复制一份 local helper contract 再自己维护”。

这条约束是为了防止一种常见退化：

- 文件是拆开了；
- 但每个文件都各自复制一点地址映射、blob 读写、或 fixed-slot 约定；
- 结果行数下降了，真实耦合反而扩散。

**Alternatives considered:**

- 允许每个 domain 本地复制少量 helper：Rejected，因为这会让 fixed-slot/ABI/addr-map contract 重新碎片化。

### 6. Structural validation must migrate together with the split

当前 `tools/check_ptx_emit_name_allowlist.py` 默认扫描 `sbt/ptx_emit.cpp`。模块化后，如果不同时迁移这个工具和相关回归，最容易发生的退化是：

- correctness authority 约束仍写在 docs/spec 中；
- 但新 lowering files 根本不在静态检查覆盖范围内；
- 结构性 regression 因而悄悄失效。

因此本 change 要求验证同步迁移：

- 静态 allowlist 覆盖完整 emitter lowering 文件集；
- 相关 poison-name/contract regressions 在 modularized emitter 上继续成立；
- `external_mnemonic_contract_test` 继续覆盖 comments / diagnostics / external symbol 等 external mnemonic contract；
- compile-first 和 existing emitter tests 继续把 modularized 实现当作一个 coherent emitter 来验证。

模块化不能以“检查脚本晚点再说”作为后补项，否则 change 的主要风险就会被放大。

**Alternatives considered:**

- 先拆代码，后续再扩静态检查：Rejected，因为这会制造一个 authority regression 可溜过的窗口期。

## Risks / Trade-offs

- `EmitCtx` 仍会是一个较重的 shared host；这不是设计缺陷，而是为了避免在同一 change 中同时做宿主抽象重写。代价是第一阶段不会把所有内部耦合都消灭。
- `vector` domain 首阶段仍可能偏大；这是有意接受的 trade-off，因为其内部共享 helper 和 review 语义边界目前还不足以安全细拆。
- 把 builtin 归 `control` 会让该域保持“调用 + 若干内联 builtin”这一混合表面；这是为了保持 ownership 一致，而不是追求语义上最纯粹的数学分类。
- 新增多个 `.cpp` / internal headers 后，构建和静态检查入口需要同步更新；这是一次性维护成本，但比继续在单文件中累积结构债更可控。
- 若实现时违反“core primitives centralized”原则，拆分会从“解耦”退化成“复制粘贴式分层”，这是本 change 需要重点防止的失败模式。
