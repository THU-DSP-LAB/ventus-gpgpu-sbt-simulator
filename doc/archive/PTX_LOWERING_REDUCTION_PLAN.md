# PTX Lowering 指令缩减优化计划

> 状态：`historical superseded planning note`
>
> 本文已从 `doc/` 迁入 `doc/archive/`。
>
> 迁移原因：
> - 这份“四问题整体计划”已不再适合作为当前工作入口；
> - problems 1 / 3 / 4 已由当前主线解决或实质性收敛；
> - problem 2 仍未解决，但已改为单独文档 `doc/ADDRESS_SPACE_SPECIALIZATION.md` 跟踪。

本文聚焦一个明确目标：**Ventus ELF 翻译到 PTX 之后，生成的 PTX 程序本身应尽量短小，每条 Ventus 指令应尽量用更少的 PTX 指令实现**。

注意：本文讨论的是 **PTX lowering 质量**，不是 `sbt_ptx` 自身的运行时开销。后者属于“翻译器执行性能”，不在本文主目标内。

> 注：problems 1 / 3 / 4 的当前实现已经切换到 replicated active-lane scalar state。本文中涉及“leader-only canonical scalar state / full-`x` broadcast”的段落，主要作为历史膨胀来源与优化演进背景保留；当前仍活跃且未被主提案取代的重点是 problem 2（地址空间专门化）。

## 1. 背景需求

当前仓库已经完成：

- Ventus ELF -> decode -> CFG verify -> PTX emit 的基本流水线；
- compile-first 与端到端联调；
- 一批 Rodinia / PoCL kernel 的 bring-up。

现阶段的下一步重点不是“先支持更多指令”，而是：

- 在保持现有语义前提下，缩短生成 PTX 的指令序列；
- 优先减少那些与 Ventus 单条指令直接相关、但在 PTX 中膨胀明显的 lowering 模板；
- 为后续的 SASS 质量提供更好的输入，而不是把大量冗余同步、访存和框架代码交给 `ptxas` 被动处理。

约束与边界：

- 不引入 silent fallback，不靠“跑起来就行”的降级路径隐藏问题；
- 仍以当前原型期语义为准；其中标量主线已切到 replicated active-lane scalar state，数值地址空间映射、PDS 语义与 direct call 支持范围保持不变；
- 优化优先顺序以“减少 PTX 指令条数”和“避免明显冗余模板”为主，不以微小 peephole 为主。

> 注：本文件的若干问题分解与“leader-only / shared regfile”表述描述的是历史膨胀来源与演进路径，不再等同于当前 `sbt/ptx_emit.cpp` 的默认实现口径。当前主线语义请以 `doc/IMPLEMENTATION_CODEMAP.md` 与 `openspec/specs/replicated-scalar-state/spec.md` 为准。

## 2. 当前最主要的 4 个膨胀源

### 2.1 问题 1：标量 `x` 寄存器当前以 per-warp shared regfile 表示

#### 现状

当前 emitter 把标量寄存器文件放在 per-warp shared memory 中：

- 读取 `x` 寄存器：`ld.shared.u32`
- 写入 `x` 寄存器：`st.shared.u32`
- 在 leader-only 标量执行模式下，写后通常追加 `bar.warp.sync`

关键位置：

- `emit_ld_x_u32_*` / `emit_st_x_u32_*`：`sbt/ptx_emit.cpp`
- `emit_st_x_u32_scalar()`：leader-only 写后会 `emit_warp_sync()`

这会让很多原本应当是“寄存器内一条完成”的标量指令膨胀成：

```ptx
@%p0 ld.shared.u32 %r14, [%rd3+8];
@%p0 add.s32 %r15, %r14, 32;
@%p0 st.shared.u32 [%rd3+8], %r15;
bar.warp.sync %r1;
```

对应的 Ventus 指令可能只是一条 `addi`。

#### 当前问题

- 冗余 `ld.shared/st.shared` 很多；
- `bar.warp.sync` 频繁出现；
- 标量链式运算无法在 PTX 寄存器里自然串起来；
- 后续很多 `vx` 指令还要再次从 shared 取一次标量源。

#### 方案计划

##### MVP

改成“leader-lane 标量寄存器为 canonical state”：

- `x-reg` 直接按全集 `x0..x255` 处理，而不是只做热点镜像；
- 函数体执行时，`x-reg canonical = leader PTX regs`；
- direct call 边界的交换格式固定为完整 `x[256]`，纳入统一 `Mutable CallState`；
- structured divergence 期间允许每条路径拥有 path-local leader；
- 在对应 `join` 的每条前驱边尾部，对该路径 active subset 广播完整 `x[256]`；
- `join` 后重新选 leader；`join` 不做 `x-state` merge；
- 第一版所有标量指令仍按 leader-only 执行；
- 当 all-lane 指令需要读取某个标量值时，用 `shfl.sync.idx` 从当前 leader 广播，而不是从 shared 再读。

##### 进阶版

- 做块级/函数级的标量寄存器缓存与脏位跟踪；
- 对 `join` 前驱边的完整 `x[256]` 广播做摘要化 / live subset 裁剪；
- 若后续有充分证据，再讨论部分纯标量指令的 all-lane 冗余执行备选方案；
- 让旧 `WarpCtx` / `wctx_ptr` 逐步退出主线。

##### 预期收益

- 大幅减少 `ld.shared.u32`
- 大幅减少 `st.shared.u32`
- 大幅减少 `bar.warp.sync`

这是当前最核心、收益最大的 PTX 缩减方向。完整可实施设计与 direct call / structured divergence 的耦合约束，见 `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`。

### 2.2 问题 2：标量/向量访存大量走通用数值地址映射模板

#### 现状

当前 `lw/sw/lb/lh/...` 与对应向量访存，大量复用统一模板：

1. 判断地址是否落在 shared 区间；
2. 判断地址是否落在 heap 区间；
3. 计算 shared pointer；
4. 计算 ELF global pointer；
5. 按 heap 条件覆写 global pointer；
6. 再执行真正的 load/store。

关键位置：

- `emit_addr_map_and_ld_u32*`
- `emit_addr_map_and_st_u32*`
- `emit_addr_map_and_ld_u8_zext_u32*`
- `emit_addr_map_and_ld_u16_zext_u32*`

#### 当前问题

- 单条 `lw/sw` 很容易展开成十几条 PTX；
- 即使某地址显然来自 `x2/x8 + imm`，仍走完整模板；
- 即使某地址显然来自参数区或 ELF backing，仍走完整模板；
- 对 Rodinia 常见栈/局部数据访存，这是明显的系统性冗余。

#### 方案计划

##### MVP

在 emitter 前增加一个轻量地址分类分析，对常见地址表达式做静态判定：

- `SharedKnown`
- `ElfKnown`
- `HeapKnown`
- `Unknown`

优先覆盖这些来源：

- `x2` / `x8` 派生的栈与 LDS 访问；
- `x10` 派生的参数区访问；
- `auipc + imm` / `lui + addi` 形成的 ELF 区访问。

当地址空间可静态确定时，直接发专门化 PTX：

- `.shared`：`add + ld.shared/st.shared`
- `.global(elf)`：`add + ld.global/st.global`
- `.global(heap)`：`add + ld.global/st.global`

只有 `Unknown` 才退回现有通用模板。

##### 进阶版

- 把地址分类提升为显式语义层；
- 对向量访存同样做地址空间专门化；
- 把 `PDS` 访问单独建模，不再通过普通 shared/elf/heap 模板旁路实现。

##### 预期收益

- 一条常见 `lw/sw` 可从十几条 PTX 缩到 1 到 3 条；
- 栈访问与参数访问收益最直接。

### 2.3 问题 3：leader 上下文当前按基本块固定重建

#### 现状

当前每个 basic block 开头都会无条件发射：

```ptx
activemask.b32 %r1;
bfind.u32 %r2, %r1;
setp.eq.u32 %p0, %r0, %r2;
```

对应实现：

- `emit_block_preamble()`
- `emit_body()` 中每个 block 都调用一次

#### 当前问题

- 即使当前 basic block 只包含纯向量算术，也会支付这 3 条 PTX 的固定税；
- 很多 block 内根本没有 leader-only 标量操作，却仍重复构造 `%p0`；
- 这些指令不对应任何具体 Ventus 指令，而是当前实现框架的附加成本。

#### 方案计划

##### MVP

在与 `value_blob` direct-call ABI 对齐后，把旧的“`%r1/%r2/%p0` 整体 leader context”拆掉，改成：

- 持久状态只保留 `owner_lane`
- `active mask` 按 use 点通过 `activemask` 读取
- owner predicate 按 use 点由 `laneid == owner_lane` 现算
- 不再在每个 block 入口固定发射 `activemask/bfind/setp`
- 旧的 `%r2 = bfind(activemask)` 退出主线语义

`use` 仍按 lowering primitive 判定，而非按 Ventus 指令类别硬编码：

- 需要当前 active subset 的 lowering，在 use 点现取 `activemask`
- 需要 owner-only 执行的 lowering，在 use 点现算 owner predicate
- 当前已知的标量条件分支 `beq/bne/blt/bge/bltu/bgeu` 使用 `bra.uni`，不属于 owner-predicate use

完整可实施设计见：`doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`

##### 进阶版

- 若后续实测表明 use 点现算 `activemask` / owner predicate 的 PTX 成本仍明显偏高，可再评估是否值得引入局部缓存；
- 结合问题 1 的标量寄存器 canonical state 与问题 4 的 `value_blob` call ABI，继续减少旧 `WarpCtx` / shared 标量路径残留；
- 对 structured divergence / `join` 边尾广播协议做更细粒度的物理优化，而不改变本文已冻结的语义边界。

##### 预期收益

- 去掉大量与真实指令无关的框架 PTX；
- 实现复杂度低于问题 1、2，适合作为早期收缩项。

### 2.4 问题 4：direct call 当前通过整块 `vctx` spill / restore 维持向量寄存器状态

#### 现状

当前 direct call 的设计目标是：**保持整套 Ventus 向量寄存器文件跨函数调用持续存在**。

做法不是把 `v0..v255` 直接变成 `.param` 逐个传递，而是：

1. 在 entry 中分配一块 `.local` 缓冲区 `__sbt_vctx[1024]`；
2. call 前把 `%v0..%v255` 全量 `st.local.u32` 到这块缓冲区；
3. `call.uni` 时把 `vctx` 指针作为一个 `.param` 传给 callee；
4. callee 入口再把 `%v0..%v255` 全量 `ld.local.u32` 读回自己的 `%v<256>`；
5. callee 返回前再把 `%v0..%v255` 全量写回 `vctx`；
6. caller 在 call 后再全量 restore。

关键位置：

- `emit_vctx_store_all()`
- `emit_vctx_load_all()`
- `emit_direct_call()`
- `emit_one_inst()` 中 call 路径

#### 一个简单例子

假设 Ventus 代码为：

```text
A:
  v5 = v1 + v2
  jal ra, B
  v8 = v5 + v3

B:
  v5 = v5 * 2
  ret
```

当前语义要求：

- `B` 能直接看到 `A` 调用点时的整套 `v-regfile`
- `B` 修改后的 `v5` 返回后仍能被 `A` 继续使用

因此当前 PTX 方案是：

```text
caller %v<256>
-> spill 到 local vctx
-> call(vctx_ptr, ...)
-> callee 从 vctx load 到 callee 自己的 %v<256>
-> callee 修改
-> callee spill 回 vctx
-> caller 再 restore 回 caller 自己的 %v<256>
```

#### 为什么当前没有直接把所有向量寄存器作为 `.param` 传递

原因不是“PTX 不支持参数传递”，而是当前语义和 ABI 设计还没有收缩到“小接口”：

- `.param` 传递的是参数空间，不等于“caller 寄存器直接借给 callee 用”；
- callee 的 `%v5` 和 caller 的 `%v5` 仍是两套独立 PTX 虚拟寄存器；
- 如果要直接传 `.param`，必须先定义：
  - 哪些 `v` 是 live-in
  - 哪些 `v` 是 live-out
  - 哪些是 caller-saved
  - 哪些是 callee-saved
  - 返回值如何编码

当前实现还没有这套 interprocedural ABI/liveness 体系，因此选择了“整块向量寄存器上下文显式保存/恢复”的方案。

另外，就算把 256 个 `v` 全写成 `.param`：

- caller 仍要 `st.param` 很多次；
- callee 仍要 `ld.param` 很多次；
- 返回方向依然要显式回传；

它并不会天然变成“零成本寄存器传递”。是否最终落成寄存器还是访存，也要由 `ptxas` 根据 PTX 结构决定。

#### 当前问题

- 有 direct call 时，一条 `jal` 可能引入数百条 `ld.local/st.local`；
- builtin 与普通 helper 的调用开销模型未分层；
- `mod.need_vctx = !funcs.empty()` 过于粗糙，只要存在 helper 函数就进入大模型；
- 小函数调用完全没有利用 live set 或 inlining 机会。

#### 方案计划

##### MVP

放弃 `vctx` 上下文 ABI，改用统一的 `value_blob` value ABI：

- `Mutable CallState` 统一覆盖：
  - `x-reg`
  - `v-reg`
  - `CSR_RPC`
  - `leader_lane`
- PTX 物理接口首版固定为：
  - `mutable_state_blob in`
  - `readonly_machine_ctx_blob in`
  - `runtime_env_blob in`
  - `mutable_state_blob out`
- 不再把 `wctx_ptr` / `vctx_base` 作为 helper ABI 的长期基石；
- divergence 区域内的 direct call 使用当前路径的 path-local `Mutable CallState`；
- call 返回后继续在当前路径内演化，直到 `join` 前驱边广播。

##### 进阶版

- 对 `mutable_state_blob` 做 live subset / callee 摘要裁剪；
- 视实测结果决定只读 `MachineContext` 字段的“显式字段 vs 现算”比例；
- 逐步让旧 `vctx` 路径退场，仅保留迁移期对照用途。

##### 预期收益

- 对存在 helper call 的 kernel，收益可能是数量级的；
- 能显著减少 `.local` 读写和 call 前后样板代码；
- 让 direct call 成本更多转化为 `ptxas` 可优化的寄存器 / spill / stack 问题。

## 3. 建议的落地顺序

如果按“收益最大”排序：

1. 问题 1：标量寄存器模型重构
2. 问题 2：访存地址空间专门化
3. 问题 4：direct call selective spill / ABI 收缩
4. 问题 3：lazy leader context

如果按“低风险先落地”排序：

1. 问题 4：callsite selective spill
2. 问题 3：lazy leader context
3. 问题 2：静态地址分类
4. 问题 1：标量寄存器模型重构

建议执行策略：

- 第一阶段先做问题 4 + 3，快速压掉明显的框架型冗余；
- 第二阶段做问题 2，把常见 `lw/sw` 模板专门化；
- 第三阶段再推进问题 1，系统性重构标量寄存器表示；
- 每阶段都配套生成真实 PTX diff，并以“PTX 指令条数、`ld/st.shared` 次数、`bar.warp.sync` 次数、`ld/st.local` 次数”作为观测指标。

## 4. 与现有文档的关系

- `doc/IMPLEMENTATION_CODEMAP.md`：记录当前实现结构与真实调用链。
- `doc/archive/IMPROVEMENT_PROPOSALS.md`：更广义的工程通用化建议快照。
- 本文：只聚焦 **PTX lowering 本身如何缩短指令序列**，并把当前明确的 4 个主要膨胀源和后续方案计划固化下来。
