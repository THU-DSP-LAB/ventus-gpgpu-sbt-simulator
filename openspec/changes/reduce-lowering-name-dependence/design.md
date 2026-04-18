## Context

本设计描述的是 **target behavior**，不是当前已完全实现的行为。

当前仓库已经具备部分结构化基础，但它们还不足以支撑“emit 整体 descriptor-driven”：

- `DecodedInst` 已携带 `inst_id`、`operand_form`、`imm_kind`、`uniform_transfer_kind`、`scalar_exec_kind`、`custom`、`mma` 等字段；
- `sbt/instruction_metadata.cpp` 已为 Spike-backed 非 custom 指令维护 repository-managed shared metadata；
- custom non-MMA decode 已产出 `custom.family/subop/dtype`；
- MMA 已基本按 `MmaInstInfo + planner / ABI descriptor` 做结构化 lowering；
- 但 `sbt/ptx_emit.cpp` 仍在 scalar branch / memory / ALU / FP / CSR、vector memory / integer / float / compare / mask / convert / move，以及 custom non-MMA 等几乎所有域里大量按 `di.name` 分派；
- `sbt/cfg.cpp` 的 basic-block terminator 分类与 edge 生成也仍保留一套 `beq/vb*/jal/jalr` 名字分类。
- `sbt/ptx_emit.cpp` 对 `setrpc/join/barrier/vsetvli/endprg` 等当前 supported structured control 语义也仍直接按 `di.name` 选 no-op / barrier / return-like 路径；
- `sbt/instruction_metadata.cpp` 中“是否需要 scalar execution classification”的边界也仍带有名字特判。

因此，本 change 不再把目标定义为旧方案中的“若干 first-stage path 降低 name 依赖”，而是明确瞄准：

- **所有 current supported emit path 的 correctness path 都必须在进入 emitter 前拿到 emit-authoritative descriptor / payload**
- **PTX emitter 不再从 `DecodedInst.name` 二次推断 ordinary/custom 语义**

本设计依赖以下 current canonical docs / specs：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`

同时，本设计显式吸收 `doc/archive/TEMP_LOWERING_OPCODE_AND_NAME_EXCHANGE_FORMAT_DISCUSSION.md` 中已经收敛清楚的结论，但不把那份归档讨论稿直接视为 current contract。

## Goals / Non-Goals

**Goals:**

- 让 **all current supported PTX emission paths** 的 authority 从 `DecodedInst.name` 挪到 decode / shared metadata 产出的结构化 descriptor / payload。
- 让 `sbt/ptx_emit.cpp` 的主分派结构按 descriptor domain / kind 组织，而不是按 mnemonic 字符串树组织。
- 让 emitter 所需的 control / structured-control 语义进入显式 contract，并用于消除 PTX emission correctness path 内的名字分派。
- 让 custom non-MMA 成为 payload-authoritative，删除 `name -> subop/dtype` fallback。
- 保持 MMA 继续按现有结构化 metadata 路线工作，并把它纳入统一 emit contract。
- 保持 `DecodedInst.name` 作为 external mnemonic contract，而不是 emitter authority。
- 让“是否需要 scalar execution classification”这条边界也转为 metadata / domain tagging authority，而不是 emitter-local 名字例外。
- 让 external mnemonic contract、结构性 fallback 删除、以及 phase exit criteria 都能被明确验证。
- 让验证与文档同步成为 change 的显式组成部分。

**Non-Goals:**

- 不要求删除 `DecodedInst.name`。
- 不要求在本 change 中移除 decode / shared metadata lookup 对 mnemonic name 的内部依赖。
- 不要求在本 change 中重写 `sbt/cfg.cpp` / `sbt/cfg_verify.cpp` 的名字分类或结构化分析逻辑。
- 不要求把 `cfg_verify` 围绕 `setrpc/join/barrier` 的全部结构化规则改写成 descriptor-first。
- 不要求引入独立于 `DecodedInst` 之外的新 IR 图结构；允许通过扩展 `DecodedInst` / shared metadata 达成目标。
- 不把上级 `ventus-env` 子项目纳入改动范围。
- 不把 unsupported/currently blocked 的指令族强行纳入 supported surface，只重构 current supported emit path。

## Decisions

### 1. Emit correctness path is fully descriptor-driven for the current supported surface

本 change 的核心决定是：

- 对于所有 current supported、可到达 PTX emission 的指令，emitter 必须在进入 lowering 前就能拿到足以决定 lowering 的显式 descriptor / payload；
- `DecodedInst.name` 不再参与 emitter correctness path 的语义判定；
- 若某条 supported-path 指令缺少所需 descriptor / payload，translation 必须显式失败。

这里的收口边界特指 **PTX emission correctness path**。本 change 不把它扩展解释为：

- decode 自身已不再依赖 mnemonic name；
- shared metadata lookup 已经不再以 name 作为内部 key；
- `cfg.cpp` / `cfg_verify.cpp` 已同步完成去 name string 化。

这些上游/旁路消费者的去字符串化属于后续 change。

这里的“fully descriptor-driven”指的是：

- emitter 允许按 `descriptor domain -> kind/subkind` 做结构化分派；
- emitter 不允许在 correctness path 中再通过 `if (di.name == "...")`、prefix/suffix 解析、或 consumer-local helper 把名字重新解释成语义。

这条规则覆盖：

- ordinary non-custom
- custom non-MMA
- 已支持 MMA
- direct jump/call/ret 的 PTX emission 语义选择
- 当前 supported structured control 指令（例如 `setrpc/join/barrier/vsetvli/endprg`）的 emitter 语义选择

但它不要求删除 external-facing `name`，也不要求新增独立 IR 图。

本 change 不把这些 structured control 语义保留成“隐式名字例外”。若某条 supported-path 指令不打算迁移到 descriptor contract，必须被显式列为例外；而本 change 的决定是把上述 structured control 指令纳入 contract，而不是豁免它们。

**Alternatives considered:**

- 只要求旧方案意义上的“已迁移路径” descriptor-driven：
  - 拒绝，因为这仍允许 emitter correctness path 在很大面积上继续依赖 `name`，达不到整体 contract 收敛。
- 继续依赖 `inst_id = hash(name)`：
  - 拒绝，因为它只改变比较载体，不改变 authority 来源。

### 2. The project needs a richer emitter-facing descriptor layer, not just a few point fixes

若目标是整体 descriptor-driven，当前 `operand_form / imm_kind / uniform_transfer_kind / scalar_exec_kind` 这组字段不够。

本设计要求补足一个 **emitter-facing descriptor layer**。它可以直接附着在 `DecodedInst` 上，也可以由 shared metadata 在 decode 时填充到 `DecodedInst`，但必须满足：进入 emitter 时已经可用。

需要覆盖的最小域包括：

- control-flow descriptor
  - `cf_kind`
  - `branch_cond_kind`
  - direct jump / direct call / ret / indirect terminator 分类
  - structured control kind（`setrpc/join/barrier/vsetvli/endprg`）
- scalar memory descriptor
  - load/store kind
  - width / sign-extension / raw-bit behavior
  - leader-only / all-lane execution requirement
- scalar integer / bitmanip descriptor
  - op kind
  - reg-reg / reg-imm form
  - signed/unsigned / shift semantics
- scalar FP descriptor
  - move / sign / minmax / cmp / convert / class / fma / unary-binary family kind
  - rounding-mode consumption rule where applicable
  - Zfinx raw-bit interpretation rule
- CSR descriptor
  - csr op kind
  - current supported CSR semantic class
- vector memory descriptor
  - ordinary global/shared vs PDS addressing kind
  - width / sign-extension / store kind
- vector arithmetic / compare / mask / convert / move descriptor
  - domain kind (`int` / `fp` / `mask`)
  - operand flavor (`vv` / `vx` / `vi` / `vf` / `mm`)
  - compare semantics / result encoding / convert semantics
- custom non-MMA payload
  - `family/subop/dtype`
- MMA metadata
  - 现有 `MmaInstInfo + planner / ABI descriptor` 继续作为该域的 canonical contract

这里不要求这些字段必须组织成一个“大一统 mega-descriptor struct”。允许分层、按域拆分；关键要求是：

- emitter 不需要再看 `name` 才能决定语义；
- descriptor 的 authority 只能来自 decode / shared metadata。

**Alternatives considered:**

- 继续以少量 point descriptor 修补 emitter：
  - 拒绝，因为那只能形成局部收口，无法支撑整体 descriptor-driven 目标。
- 强制引入一个单一 mega-descriptor：
  - 拒绝，因为可能制造僵硬抽象；按域拆分更贴近现有代码结构。

### 3. Emitter dispatch is reorganized by semantic domain, not by mnemonic text

`sbt/ptx_emit.cpp` 的目标结构不再是：

- `if (di.name == "...")`
- `else if (di.name == "...")`
- `name` prefix/suffix helper

而是按 semantic domain / kind 组织，例如：

- control-flow lowering
- scalar memory lowering
- scalar integer lowering
- scalar FP lowering
- CSR lowering
- vector memory lowering
- vector integer/mask lowering
- vector FP/convert lowering
- custom non-MMA lowering
- MMA lowering

每个 lowering helper 的入口 guard 都必须来自 descriptor / payload，而不是来自 `name`。helper 内允许使用 descriptor 枚举或 kind 做结构化分支；但不允许再从 `name` 重建语义。

这样做的收益是：

- emitter 结构与语义域对齐，而不是与 mnemonic 字符串列表对齐；
- 新增 supported 指令时，优先补 descriptor / metadata，再接入对应 domain helper；
- 可以把“支持某类语义”与“支持某个名字字符串”区分开。

**Alternatives considered:**

- 继续保留现有大号字符串树，只把少数 helper 改成 descriptor：
  - 拒绝，因为这会让 authority 边界长期模糊。

### 4. Emitter control selection becomes structured-first, while CFG remains on the current contract

本 change 允许为 emitter 建立或扩展 control descriptor，但不要求 `cfg.cpp` / `cfg_verify.cpp` 同步切换到 descriptor-first。

- decode / shared metadata 可以按当前实现习惯继续产出 emitter 所需字段；
- emitter 的 control-flow lowering 选择
- emitter 对 `setrpc/join/barrier/vsetvli/endprg` 的当前 supported 语义选择

都必须基于前置 `cf_kind + branch_cond_kind + related call/ret info + structured control kind` 或等价 structured metadata。

但以下内容仍不在本 change 范围内：

- `cfg.cpp` 的 branch / jump / call / ret 名字分类重写
- `cfg_verify` 对 `setrpc/join/barrier` 的全部结构化规则重写
- 向量 uniform 分析的整体抽象升级

原因是：

- 当前 change 的目标是先收紧 emitter correctness path；
- 若把 CFG 一并强制去字符串化，会显著扩大 change，模糊本次交付边界。

### 5. Custom non-MMA must become payload-authoritative with no semantic fallback

custom non-MMA 已经具备最成熟的结构化基础，因此必须彻底完成 payload-authoritative：

- `shuffle` 只看 `family/subop` 与显式操作数字段
- `vcvt` 只看 `family/subop/dtype`
- packed arithmetic 只看 `family/subop/dtype`
- SFU 只看 `family/subop/dtype`

不允许继续保留：

- `name -> subop`
- `name -> dtype`
- 通过 mnemonic stem/prefix/suffix 选 lowering 分支

若 payload 缺失或不完整，必须显式失败。

这里的正确性保障主要仍来自：

- decode contract tests
- emitter/compile-first tests
- semantic oracle

一致性 assert 只能是附加检查，不是主要验证机制。

### 6. MMA stays structured and is treated as already aligned with the target contract

MMA 当前已经是结构化路径：

- decode 产出 `MmaInstInfo`
- lowering 按 `support_class / lowering_class`
- planner / ABI helper 承载 tuple/tile 细节

本 change 不重写 MMA。相反，它将 MMA 明确标记为：

- 当前已满足“descriptor-driven emit”方向的结构化正例；
- ordinary/custom non-MMA 需要向其靠拢，而不是把 MMA 压回字符串或扁平 enum 分派。

### 7. `DecodedInst.name` remains an external contract only

`DecodedInst.name` 仍保留给：

- `sbt_decode pretty`
- JSON / diagnostics
- coverage / mnemonic reporting
- 某些 external-facing builtin symbol / diagnostic text

但它只能承担 external contract，不再承担 emitter authority。

这意味着：

- rename / alias / output formatting 的调整，不应改变 emitter correctness；
- 若某条 emit path 仍需要依赖 `name` 才能决定语义，则该路径尚未完成本 change。

此外，external mnemonic contract 本身也必须被显式回归，而不是默认假设“去 authority”不会影响 pretty / JSON / coverage / builtin symbol / diagnostics。

### 8. Scalar execution classification applicability must also stop depending on mnemonic exceptions

本 change 不只要求 scalar-side lowering helper 改成 descriptor-driven，也要求：

- “某条指令是否要求 scalar execution classification” 这条边界来自显式 metadata、descriptor domain，或等价的 repository-managed tagging；
- 不允许继续通过 emitter-local helper 按 `jal/jalr/vmv.x.s/...` 名字例外决定 applicability；
- `vmv.x.s`、scalar branch、scalar memory、scalar FP、CSR 等 supported scalar-side path，必须通过显式 domain / descriptor / classification contract 决定是否走 all-lane、leader-only 或 fixed-lane 语义。

这项收口的目标是避免“lowering 主体已 descriptor-driven，但 classification boundary 仍由名字决定”的隐蔽双事实源。

### 9. Verification must prove semantic preservation, fallback removal, and external-contract preservation

本 change 不能只靠“语义 oracle 通过”来验证，因为 oracle 可以证明输出没变，却不能证明 emitter 内 name fallback 已被删净。

因此验证必须同时覆盖三类目标：

- semantic preservation
- structural contract enforcement
- external mnemonic contract preservation

最低要求包括：

- decode / metadata contract tests
- control / structured-control descriptor contract tests for emitter inputs
- emitter / compile-first tests
- custom oracle / ordinary affected regressions
- **negative tests**：缺 descriptor / 缺 payload 时显式失败
- **static structural allowlist checks**：把允许读取 `di.name` 的位置限制在 comments / diagnostics / external reporting / builtin symbol 等显式白名单中
- **dynamic poison-name checks**：在测试中故意污染 supported-path 指令的 `DecodedInst.name`，证明 lowering authority 仍完全来自 descriptor / payload
- **external contract regression**：证明 pretty / JSON / coverage / builtin symbol / diagnostics 继续暴露 canonical mnemonic contract

结构性检查不要求“文件内零 `di.name` 字符串”。允许保留：

- comments
- diagnostics
- pretty / debug text
- external builtin symbol / reporting 场景

但不允许在 supported emit path 的 correctness branch 中继续以 `di.name` 做语义分派。

### 10. Inventory and phase exit criteria are part of the change contract

本 change 需要维护一份 checked-in inventory / allowlist，至少记录：

- 每个 current supported name-dependent site 所在文件与函数
- 它属于哪个语义域
- 计划由哪个 descriptor / payload 替换
- 迁移后残余允许保留的 `name` 用法是什么

这份 inventory 可以包含 decode / CFG 的现存 name-dependent site 作为背景信息，但当前 change 的完成判定只要求清零 **emitter correctness path** 对应的 supported-path authority site。decode / CFG site 在本 change 中应被显式标注为 deferred。

每个 implementation phase 的退出条件至少包括：

- inventory 中对应语义域的 supported-path site 已清零或被显式迁入 allowlist 的非-authority 用法
- 所需 descriptor / payload 已在 decode / shared metadata 层落地
- 缺 descriptor / payload 的负测已补齐
- 该 phase 影响范围内至少一条代表性 compile-first / contract / oracle 回归通过
- 若 phase 改变了 current / active 边界，相关文档与 spec 已同步

## Implementation Phases

### Phase 1. Inventory and descriptor model freeze

- 清点 `ptx_emit.cpp` 中所有 current supported name-driven lowering sites
- 按语义域分组
- 产出 checked-in inventory / allowlist，并把 structured control 与 scalar classification applicability helper 一并纳入
- 冻结 emitter-facing descriptor 分层与字段边界
- 明确哪些字段放在 `DecodedInst`，哪些由 shared metadata 产出并写回 `DecodedInst`

### Phase 2. Control-flow and scalar-side conversion

- control-flow descriptor
- structured control kind 与其当前 supported emitter semantics
- scalar memory descriptor
- scalar integer descriptor
- scalar FP descriptor
- CSR descriptor
- scalar execution classification applicability boundary
- emitter-side control / structured-control lowering 切换

### Phase 3. Vector ordinary conversion

- vector memory / PDS descriptor
- vector integer / compare / mask / move / convert descriptor
- vector FP / compare / convert descriptor

### Phase 4. Custom non-MMA cleanup and emitter-wide fallback removal

- custom non-MMA 全量 payload-authoritative
- 删除 `name -> subop/dtype` fallback
- supported emit path 的 name-driven correctness branch 清零

### Phase 5. Verification and documentation sync

- 运行 contract tests / compile-first / oracle
- 补充负测、static allowlist、dynamic poison-name 与 external mnemonic contract regression
- 同步 README / Code Map / doc 索引 / OpenSpec 索引与 current specs

## Risks / Trade-offs

- 这是比原方案更大的 change；需要更完整的 descriptor 设计与更大面积的 emitter 重组。
- 若 descriptor 分层定义得过于模糊，会导致“名义上 descriptor-driven，实际上语义仍散落在 helper 里”的伪收口。
- 若强行追求单一 mega-descriptor，可能制造错误抽象；因此本设计允许按语义域分层。
- 若只做正向语义回归，不做负测与结构性检查，容易保留隐蔽的 name fallback。
- 若不把 special control 与 scalar classification applicability 一并纳入 contract，会留下“主路径已 descriptor-driven、边界 helper 仍 name-driven”的隐蔽双事实源。
- `cfg_verify` 仍保持当前主线，意味着本 change 只统一 emitter-side control authority，不解决更宽的 structured divergence 验证重构问题。
- 在实现完成前，active artifacts 与 current code 会显著不一致；因此文档必须严格区分 `current` 与 `active`。
