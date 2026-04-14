## Context

> Status: `active`
>
> This document describes the target behavior for the `introduce-ptx-virtual-temp-registers` change. It does not describe current implemented behavior unless explicitly labeled as such.

当前实现真相以 `doc/IMPLEMENTATION_CODEMAP.md` 和 `sbt/ptx_emit.cpp` 为准。当前 PTX emitter 在函数入口统一声明：

- `%r<32>` / `%rd<32>` / `%p<16>`
- `%f<16>` / `%h<16>` / `%ub<4>` / `%uh<16>`
- `%x<256>` / `%v<256>`

其中一部分 `%r/%rd/%p/%f/...` 槽位承载固定 machine/runtime 语义，例如 `laneid`、`leader_lane`、`global_base`、`warp_id_in_block`、PDS metadata；另一部分则被多个 helper 约定为 scratch 池。当前问题不在于 PTX 语法本身，而在于 scratch 所有权仍隐式耦合在多个 lowering helper 中。

本设计依赖以下 canonical docs：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/specs/replicated-scalar-state/spec.md`
- `openspec/specs/ptx-call-prototype/spec.md`
- `openspec/changes/introduce-ptx-virtual-temp-registers/specs/ptx-temp-register-allocation/spec.md`

## Goals / Non-Goals

**Goals:**

- 为 PTX emitter 引入唯一命名、按类型分配的虚拟临时寄存器分配器。
- 将当前 scratch 使用从“默认占用固定编号槽位”迁移为“显式申请 `%tmp*` 临时寄存器”。
- 保留当前固定语义寄存器槽位、`%x/%v` 逻辑寄存器文件、direct-call value ABI 与 machine/runtime context 合同不变。
- 在不重构整体 CFG / label 发射结构的前提下完成首阶段迁移。
- 同步更新文档，使“固定槽位 vs `%tmp*` scratch”边界成为 current 维护口径。

**Non-Goals:**

- 不重新设计 direct-call value ABI 或 blob layout。
- 不重排当前所有固定语义寄存器编号。
- 不在本 change 中把 emitter 全面改写为大量块级 `{}` 局部 scope。
- 不把 `%x/%v` 逻辑寄存器文件纳入虚拟临时寄存器分配器。

## Decisions

### 1. 引入按类型分配的函数级 VirtualTempAllocator

`EmitCtx` 新增一组按类型计数的临时寄存器分配接口，例如：

- `tmp_b32()`
- `tmp_b64()`
- `tmp_pred()`
- `tmp_f32()`
- `tmp_b16()`
- `tmp_u8()`
- `tmp_u16()`

每次分配返回一个唯一命名的 PTX register identifier，例如 `%tmp_b32_17`、`%tmp_p_4`。同时 `EmitCtx` 记录本函数实际申请过的临时寄存器集合，在函数 prologue 统一输出对应 `.reg` 声明。

这样做的核心目的是把 scratch ownership 从“helper 默认共享固定槽位”改为“helper 显式申请自己需要的 temp”。

**Rationale:**

- 与当前 `emit_body()` 结构兼容，落地成本最低。
- 不需要先重写 basic-block emission、label flow 或 branch helper。
- 直接解决“误复用固定 scratch 编号”的主要风险。

**Alternatives considered:**
- 直接保留固定编号 scratch 池，只补注释：Rejected，因为无法从结构上防止误覆盖。
- 统一用一个不分类型的 `%tmpN` 池：Rejected，因为 `.b32/.b64/.pred/.f32/...` 类型边界仍会混乱，且声明生成更脆弱。

### 2. 保留当前固定语义寄存器槽位，不做首阶段重排

当前具有固定含义的槽位继续保留，例如：

- `%r0` / `%r1` / `%r2`
- `%r10` / `%r12`
- `%r26..%r30`
- `%p0`
- `%rd0` / `%rd2` / `%rd4`
- `%x<256>` / `%v<256>`

这些寄存器要么承载当前 machine/runtime contract，要么已经被 current 文档明确写死。首阶段引入 `%tmp*` 的目标是迁出 scratch ownership，而不是把整个 emitter 的固定槽位重新编号。

**Rationale:**

- 避免把 scratch refactor 扩大成 ABI/contract 重写。
- 减少对 prologue、call marshal、CSR lowering、PDS 路径的无关扰动。
- 文档同步成本更低，current truth 也更清晰。

**Alternatives considered:**
- 顺手重排所有 `%r/%rd/%p` 编号：Rejected，因为收益主要是“看起来更整齐”，而不是正确性或维护性关键路径。

### 3. 首阶段仍采用函数级 `.reg` 声明，不强制引入块级局部 scope

虽然 PTX 允许在同一词法作用域内交错 `.reg` 声明与指令，但本 change 不把“广泛引入块级 `{}` scope”作为第一阶段方案。

首阶段实现方式为：

- helper 申请 `%tmp*`
- `EmitCtx` 汇总声明
- 在函数 prologue 统一输出 `.reg`

后续若某些 straight-line helper 需要更强的局部可读性，可再独立评估是否局部引入 `{}` + 局部 `.reg`，但那不属于本 change 的必要条件。

**Rationale:**

- 当前 emitter 依赖 basic-block label emission；许多 helper 还会自行生成 label/branch。
- 若在首阶段同时引入广泛词法作用域，容易把 scratch refactor 变成控制流结构改造。
- 先完成唯一命名 temp allocator，已经能解决绝大多数误复用问题。

**Alternatives considered:**
- 首阶段全面改成块级局部 `.reg` 声明：Rejected，因为会明显扩大改动面，并增加 label/scope 交互复杂度。

### 4. 迁移按“高风险 scratch helper 优先”分批进行

迁移优先级按 scratch 冲突风险和 helper 复用密度排序：

1. 地址映射 helper
2. scalar FP lowering
3. builtin lowering
4. direct-call marshal 周边临时值
5. custom non-MMA lowering
6. MMA scratch / tuple construction path

每一类迁移时，应把 helper 依赖的固定 scratch 改为显式 temp 参数或局部 temp 申请，而不是继续从外部假定“某几个 `%r/%p` 没人在用”。

**Rationale:**

- 地址映射和 builtin helper 被多条路径复用，最容易成为隐式共享 scratch 槽位。
- MMA 与 custom 路径更复杂，适合在 allocator 机制稳定后迁移。

**Alternatives considered:**
- 一次性全文件替换所有 scratch 编号：Rejected，因为难以审查，也不利于定位回归。

### 5. 文档中明确区分固定槽位与 `%tmp*` scratch

本 change 落地后，以下文档必须同步：

- `README.md`：用户视角的 PTX emitter 寄存器口径
- `doc/IMPLEMENTATION_CODEMAP.md`：当前实现真相
- `openspec/README.md`：active change 导航

文档必须显式区分：

- `current`：哪些寄存器槽位仍是当前固定 machine/runtime contract
- `active`：`%tmp*` allocator change 仍在推进的设计与任务
- `historical`：旧的固定 scratch 池讨论只保留为背景，不再作为目标设计

**Rationale:**

- 本 change 的收益很大一部分来自维护口径收敛，而不是仅靠代码改动。
- 如果文档不更新，维护者仍会继续把 scratch 当成固定编号池使用。

**Alternatives considered:**
- 只改代码，不改 current 文档：Rejected，因为会违反仓库的文档同步约束，也会让新旧维护口径并存。

## Risks / Trade-offs

- `%tmp*` 唯一命名会让生成的 PTX 文本更长；这是可接受成本，换来更清晰的 scratch ownership。
- 若 helper 迁移不彻底，可能出现“固定 scratch + `%tmp*` 混用”的过渡状态；这需要在任务拆分和验证阶段显式检查。
- 虚拟临时寄存器唯一命名不等于自动降低物理寄存器占用；最终寄存器压力仍由 `ptxas` 的 live-range 分析决定。
- 首阶段保留函数级声明意味着 temp lifetime 主要是语义上的、不是词法块上的；这足以解决 ownership 问题，但不是最强的可读性终态。
