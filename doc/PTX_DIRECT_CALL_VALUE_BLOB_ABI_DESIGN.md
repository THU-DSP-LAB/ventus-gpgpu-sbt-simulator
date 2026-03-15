# PTX Direct Call `value_blob` ABI 设计

本文给出 direct call 从当前 `vctx` context ABI 迁移到 `value_blob` value ABI 的正式设计方案。本文只定义语义边界、状态分层、PTX 调用接口约定与正确性前提，不包含实现代码。

目标读者是刚接触本项目的维护者。假定读者已经阅读：
- `doc/IMPLEMENTATION_CODEMAP.md`
- `doc/PTX_LOWERING_REDUCTION_PLAN.md`
- `sbt/ptx_emit.cpp`

在此基础上，读者应当能够仅凭本文理解：
- 当前 direct call / `x-reg` 方案为什么需要一起重构；
- 为什么新方案选择 `value_blob` + `leader PTX regs`；
- structured divergence / `join` 下 `x-reg` 如何保持正确；
- 以及实现时哪些边界是固定设计，哪些仍是可调物理布局。

相关背景材料：
- 缩减动机与问题定义：`doc/PTX_LOWERING_REDUCTION_PLAN.md`
- 源码现状与调用链：`doc/IMPLEMENTATION_CODEMAP.md`
- 讨论纪要：`doc/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`
- 外部实验报告：`../sbtsim_lab/lab/05_ptx_direct_call_param_abi/EXPERIMENT_REPORT.md`

## 1. 目标

本设计的目标是：
- 用统一的 value ABI 取代当前 direct call 上的 `vctx` 整块 `.local` spill / restore 模型；
- 把 caller / callee 之间需要显式传递的 Ventus 状态统一建模，而不是继续由 `x-reg` / `v-reg` / CSR 各自独立处理；
- 统一收敛问题 1 中 `x-reg` 的表示、call 边界交换格式与 structured divergence 下的 owner 规则；
- 尽量把“物理寄存器 vs spill / stack / local”这类后端决策交给 NVIDIA `ptxas` / JIT；
- 避免 `value_params` 那种大量离散参数触发 `ptxas disabling ABI` 的路径，优先采用聚合对象（`value_blob`）方案。

本设计不追求：
- 设计完整的 caller-saved / callee-saved ABI；
- 在本阶段做 helper 摘要裁剪或 interprocedural liveness；
- 解决非 direct call / 间接调用 / 递归调用。

## 2. 当前现状与选择理由

### 2.1 当前主线现状

当前主线 direct call 采用 context ABI：
- entry 中为每线程分配 `.local __sbt_vctx[1024]`；
- caller call 前全量 `st.local` 保存 `%v0..%v255`；
- `call.uni` 传入 `vctx` 指针以及若干环境参数；
- callee 入口全量 `ld.local` 恢复 `%v0..%v255`；
- callee 返回前再全量写回，caller 返回后再次全量恢复。

该模型的问题是：
- 单次 helper call 固定引入大量 `.local` 读写；
- `need_vctx` 当前是模块级粗开关；
- builtin 与普通 helper 的成本模型没有完全分层；
- 该模型主要是在前端显式固定成本，而不是给后端优化留空间。

### 2.2 为什么选择 `value_blob`

已有实验结论支持：
- `value_params` 会触发 `ptxas disabling ABI`，不适合作为正式主线；
- `value_blob` 在 `hot4` / `full` 场景下都比 `vctx` 更有希望把成本转化为后端可优化的寄存器 / spill / stack 问题；
- `value_blob` 的 PTX 文本规模接近 `vctx`，明显优于完全分散的 `value_params`。

因此本设计选择：
- 放弃 `value_params` 作为主方案；
- 采用聚合对象 value ABI；
- 继续保留 `vctx` 仅作为迁移期对照/回退路径，而不是长期目标。

## 3. 总体设计

### 3.1 基本思路

新的 PTX direct call 模型分为两个层次：

1. **Ventus 状态模型**
   - 定义哪些状态需要跨 call 显式保持；
   - 定义这些状态在 caller / callee 边界上的 canonical representation。

2. **PTX 物理调用接口**
   - 用聚合 `blob` 形式把上述状态交给 callee；
   - 不假设 PTX 存在字面意义上的“inout blob”语法；
   - 允许实际实现落成“输入 blob + 输出 blob”或其他等价 PTX 形状。

### 3.2 设计原则

- 统一：`x-reg` / `v-reg` / 可变 CSR 不再分裂成多套独立 call 机制。
- 分层：Ventus machine state、只读机器上下文与宿主运行时环境必须区分。
- 显式：需要跨 call 显式保持的状态必须有清晰的 canonical representation。
- 最小假设：不假设 `ptxas` 会自动发明新的调用约定；只假设其能对给定 ABI 做寄存器分配与优化。
- 不引入 silent fallback：若设计前提不满足，应明确 fail-fast。

## 4. 状态分层

本设计把 PTX direct call 涉及的状态分成 4 类。

### 4.1 Mutable CallState

这部分是统一 call ABI 的核心，必须跨 caller / callee 显式保持。

包含：
- `x-reg file`
  - 逻辑上固定为完整 `x[256]`
- `v-reg file`
- 可变且程序可观察的 CSR
  - 当前至少包括：`CSR_RPC`
- `leader_lane`
  - 它属于 mutable metadata，因为会随执行变化，且在本设计中参与 `x-reg` canonical owner 语义

不包含：
- `active mask`
- PTX scratch regs
- 单纯由当前实现 convenience 引入的中间量

说明：
- `Mutable CallState.x` 的 **call 边界交换格式** 固定为逻辑 `x[256]`；
- 函数体执行过程中，`x-reg` 的 **运行时 canonical form** 不是 blob 数组本身，而是当前 owner lane 上的 PTX 标量寄存器。

### 4.2 ReadOnly MachineContext

这部分属于 Ventus machine model，但在一次 kernel / warp 生命周期中视为只读。

当前设计将下列内容归入此类：
- `CSR_NUMW`
- `CSR_NUMT`
- `CSR_KNL`
- `CSR_LDS`
- `CSR_PDS`
- `CSR_WID`
- `CSR_WGID`
- `CSR_GDX`
- `CSR_GDY`
- `CSR_GDZ`

说明：
- `CSR_WID/WGID/GD*` 在现实现中很多可以由 PTX builtin 或简单线性化重建；
- 第一版设计允许它们逻辑上属于只读 machine context，但物理实现上按“现算”或“显式字段”二选一，不要求一开始就统一成同一种方式。

### 4.3 ReadOnly RuntimeEnv

这部分不是 Ventus machine state，而是 SBT/PTX 运行时环境。

当前至少包括：
- `elf_base`
- `heap_base`
- 其他用于 backing / host launch / 地址映射的只读参数

说明：
- 它们属于 PTX call ABI；
- 但不属于 Ventus 架构状态；
- 设计文档和代码注释中必须明确区分。

### 4.4 Implicit ExecState

这部分是执行过程中的隐式状态，不纳入第一版 `value_blob` 显式序列化对象。

当前包括：
- `active mask`
- PTX / NVIDIA SIMT 执行过程中隐式维持的收敛 / 发散执行上下文
- 由 `active mask` 派生的当前 leader 选择依据

说明：
- 这些状态某种程度上属于 Ventus/SIMT 机器语义；
- 但在当前方案中依赖 PTX direct-call / 硬件 SIMT 执行语义隐式延续；
- 第一版不把这部分再编码进 blob，避免“数据里一份、硬件执行里又一份”的重复真值来源。

## 5. `x-reg` 设计约束

### 5.1 前提

本设计依赖一个重要前提：

> Ventus 编译器层已经保证：标量 `x-reg` 不会在分歧路径上形成多个需要在 `join` 合并的逻辑副本。

等价地说：
- 任意时刻只有一份逻辑上的 warp-uniform `x-state`；
- structured divergence 区域内允许出现按路径分裂的执行副本，但这些副本不会在 `join` 后形成需要 merge 的多个 live-out `x-state`；
- 不需要在 `join` 点做 `x-state` 语义 merge。

若该前提不成立，本设计必须重新审视 `x-reg` canonical 方案。

实现约束：
- 可以把该前提作为文档中显式声明的信任边界；
- 若实现阶段能在翻译期做出足够保守的 fail-fast 检查，则应尽量检查；
- 若检查成本过高，第一版允许直接信任编译器前提，但代码注释中必须明确写出这一点。

### 5.2 当前选择

本设计当前接受下列路线作为问题 1 与统一 call ABI 的耦合前提：

> `x-reg canonical = leader PTX regs`

这意味着：
- `x-reg` 的逻辑真值在函数体执行时驻留于当前 owner leader lane 的 PTX 标量寄存器中；
- `Mutable CallState.x` 在 call 边界上的交换格式固定为完整 `x[256]`；
- `leader_lane` 必须成为 `Mutable CallState` 的一部分，并通过 call ABI 显式传递；
- 第一版所有标量指令仍按 leader-only 方案执行，不采用“所有 active lanes 冗余执行标量”的主线策略。

### 5.3 为什么 `active mask` 不入 blob

虽然 `active mask` 可以视为机器状态的一部分，但本设计不把它显式纳入 `value_blob`，原因是：
- 当前 PTX 执行中的真实 active mask 已由硬件 / PTX 执行模型隐式保持；
- 通过 `activemask` 指令可在需要时读取当前真值；
- 若同时把其作为 blob 字段显式保存，容易出现“显式数据值”和“真实执行上下文”两份来源。

因此第一版设计要求：
- `active mask` 通过 `activemask` 获取；
- 不在 blob 中序列化；
- 代码实现与注释中必须明确说明这一点。

### 5.4 `leader_lane` 的语义

在本设计中，`leader_lane` 表示当前 `x-state` 的 canonical owner 身份。

因此：
- 它不是简单的“当前 active mask 中第一个 lane”缓存；
- 它也不是“每条标量 PTX 指令都必须唯一执行者”的同义词；
- 它首先是 call ABI / structured divergence 下的状态身份字段。

对 direct call 而言，正确性要求是：
- 若 call ABI 显式传递 `leader_lane`，callee 入口应优先恢复 caller 传入的 owner，而不是直接按 `activemask+bfind` 重新猜测；
- callee 返回时，返回的 `leader_lane` 与 `Mutable CallState.x` 必须共同描述调用后的新 canonical owner 与新逻辑 `x-state`；
- direct call inside divergence path 时，使用的是该路径当前 path-local owner，而不是 warp 入口时的历史 owner。

因此在本设计中：
- `leader_lane` 是 mutable metadata，不是只读上下文；
- `leader` 既不是纯执行代理，也不是只读环境字段，而是 `x-state` owner 身份的一部分。

### 5.5 Structured Divergence 下的 `x-reg` 协议

本设计不把 `join` 视为 `x-reg` 的保存/恢复点，也不在 `join` 上做语义 merge。

第一版 structured divergence 协议固定为：

1. 进入某个 `vbranch -> join` 结构化区域前，当前 owner lane 持有该区域入口的 `x-state`。
2. 进入 then/else 路径后，每条路径选取自己的 path-local leader。
3. 路径内部所有标量指令仍按 leader-only 执行；若路径内发生 direct call，则该 call 使用当前路径的 path-local `Mutable CallState`。
4. 在该 `join` 的每一条前驱边尾部，由当前路径的 path-local leader 把该路径的最终 `x-state` 广播给该路径当前 active subset。
5. 第一版广播对象固定为完整 `x[256]`，不做 live subset 裁剪。
6. 进入 `join` 后，从当前 active lanes 中任选一个作为新的 canonical owner；`join` 本身不负责 merge。

这个协议依赖 5.1 的编译器前提：
- 若某个逻辑值会在 then/else 两边变成不同的 live-out 结果，则它不应继续作为跨 `join` 的 `x-reg` 活下去；
- 因此凡是在 `join` 后仍按 `x-reg` 读取的状态，经过 4) 的路径末尾广播后，在 reconverged active lanes 上应当已经一致。

### 5.6 嵌套 Structured Divergence

嵌套 `vbranch/join` 不要求维护运行时 `path-context` 栈。

第一版只要求：
- 编译期 / emitter 能识别结构化 region 的嵌套关系；
- 对每个 `join`，都能找到其所有前驱边并插入完整 `x[256]` 广播；
- 内层 `join` 完成后，外层路径只保留“当前 active subset 上已经一致的 `x-state` + 一个重新选出的当前 owner”。

因此：
- 运行时不需要恢复“外层旧 leader 身份”；
- 内层 `join` 后只保留一个新的当前 owner，即可继续执行外层路径。

## 6. `value_blob` 逻辑模型

### 6.1 逻辑统一、物理分段

本设计采用：

> 逻辑统一、物理分段

即：
- 逻辑上存在一个统一的 PTX direct-call ABI；
- 物理上允许将其组织成少量聚合对象，而不是单个不可分割 mega-blob。

第一版建议至少分为：
- `mutable_state_blob`
- `readonly_machine_ctx_blob`
- `runtime_env_blob`

允许后续在实现阶段把其中某些部分继续拆成 `x-state` / `v-state` / `csr-state` 子段，但不改变逻辑 ABI 分层。

### 6.2 为什么不强制单 blob

不强制单 blob 的原因是：
- 便于布局与对齐控制；
- 便于把 `RuntimeEnv` 与 Ventus machine state 分离；
- 便于 future proof，例如只读字段扩展而不影响 mutable state；
- 更利于后续实测不同物理布局对 `ptxas` 的影响。

## 7. PTX call ABI 约定

### 7.1 逻辑接口

逻辑上，一个 helper call 接收：
- 调用前 `Mutable CallState`
- 只读 `MachineContext`
- 只读 `RuntimeEnv`

并产生：
- 调用后 `Mutable CallState`

### 7.2 物理接口

第一版 PTX 物理接口选择为：

- `mutable_state_blob in`
- `readonly_machine_ctx_blob in`
- `runtime_env_blob in`
- `mutable_state_blob out`

即：
- 使用聚合 blob / 聚合对象承载状态；
- 不再走大量离散 `.param .b32` 的 `value_params` 风格；
- 不采用“context pointer + callee 就地改写”的旧 `vctx` 心智模型作为主线语义；
- 允许实现时在 PTX 语法层落成“输入 blob + 输出 blob”的等价组织，但不改变这一逻辑接口。

选择理由：
- `Mutable CallState` 的交换边界清晰；
- 与“把寄存器分配 / spill 交给 `ptxas`”的目标一致；
- 避免重新把新方案退化成旧 context ABI 的变体。

## 8. 与现有 helper 参数的映射关系

当前 helper `.func` 入口里的这批参数：
- `elf_base`
- `heap_base`
- `wctx_ptr`
- `lds_ptr`
- `knl_vaddr`
- `pds_base_vaddr`
- `pds_size_per_thread`
- `warp_id`
- `warps_per_block`
- `vctx_base`

在新设计中的去向为：

- `elf_base` / `heap_base`
  - 归入 `RuntimeEnv`

- `knl_vaddr`
  - 长期语义上归入 `ReadOnly MachineContext`（对应 `CSR_KNL`）

- `pds_base_vaddr` / `pds_size_per_thread`
  - 作为构造 `CSR_PDS` 所需上下文的一部分，归入 `ReadOnly MachineContext`

- `warp_id` / `warps_per_block`
  - 长期语义上归入 `ReadOnly MachineContext`（`CSR_WID` / `CSR_NUMW`）或改为现算

- `wctx_ptr`
  - 不再作为统一 call ABI 的核心；
  - 它反映的是当前“标量 regfile 在 per-warp shared”这一旧实现事实；
  - 若 `x-reg` 已纳入统一 `CallState`，则其在 helper call ABI 中应降级甚至消失

- `lds_ptr`
  - 当前主要是 emitter convenience 字段；
  - 长期应由 `CSR_LDS` / 只读 machine context 语义替代，而不是作为独立 helper ABI 基石

- `vctx_base`
  - 作为旧 `vctx` 模型的一部分，目标是退出主线 ABI

## 9. 语义边界

### 9.1 本设计显式处理的状态

显式处理：
- `x-reg`
- `v-reg`
- `CSR_RPC`
- `leader_lane`
- 只读 machine context
- 只读 runtime env

### 9.2 本设计不显式序列化的状态

第一版不显式序列化：
- `active mask`
- PTX / 硬件隐式收敛状态
- `leader` 的派生辅助信息（除 owner lane 本身）
- PTX scratch regs

### 9.3 本设计暂不解决的问题

- helper 摘要裁剪
- 通用间接调用 ABI
- 递归 / 互递归
- 完整 caller-saved / callee-saved 约定
- 不依赖 compiler 保证的 `x-reg` 分裂/合并问题

### 9.4 第一版标量执行策略

第一版固定采用：

- 所有标量指令均按 leader-only 方案执行；
- structured divergence 区域内也不引入“所有 active lanes 冗余执行标量”的主线策略；
- `all-lane` 标量执行只作为未来可选优化方向，不属于当前正式方案。

这样做的理由是：
- 与外部对 SGPR / `x-reg` 的直觉模型更一致；
- divergence 区域内 direct call 更容易沿 path-local `Mutable CallState` 解释；
- 避免在第一版语义模型中同时引入“leader-only”和“all-lane 冗余执行”两套标量执行规则。

## 10. 实现阶段必须写清楚的注释要点

实现和代码注释中必须明确写清：

1. `RuntimeEnv` 属于 PTX call ABI，但不属于 Ventus machine state。
2. `active mask` 不显式保存在 blob 中，理由是它可由 `activemask` 读取，且依赖 PTX 硬件执行上下文隐式保持。
3. `leader_lane` 是 mutable metadata，因为它参与 `x-reg canonical owner` 语义。
4. 本方案依赖编译器保证：`x-reg` 不会在分歧路径上形成需要 `join` 合并的多个逻辑副本。
5. structured divergence 下，在每个 `join` 的前驱边尾部要做完整 `x[256]` 广播；`join` 本身不负责 merge。
6. divergence 区域内的 direct call 使用当前路径的 path-local `Mutable CallState`。
7. 若 4) 的前提在未来失效，必须重新审视 `x-reg canonical = leader PTX regs` 方案。

## 11. 未决事项

虽然总体方向已定，但以下点在实现前仍需进一步确认：

- `ReadOnly MachineContext` 中哪些字段第一版选择“显式字段”，哪些选择“现算”；
- 若实现阶段尝试补充 fail-fast 检查，哪些 structured divergence 情况能在翻译期保守验证；
- `x-reg` 进入 call ABI 后，旧 `WarpCtx` / `wctx_ptr` 何时退场；
- `value_blob` 与旧 `vctx` 路径的共存和切换策略；
- 完整实现后需要配套哪些 micro-test / runtime validation。

## 12. 结论

本设计确定：
- direct call 主线迁移方向为 `value_blob`，而非 `value_params`；
- call ABI 要统一覆盖 `x-reg`、`v-reg`、可变 CSR、只读机器上下文与运行时环境；
- `RuntimeEnv` 需要进入 PTX call ABI，但必须与 Ventus machine state 分层；
- `active mask` 不显式进入 blob；
- `leader_lane` 进入 mutable metadata；
- `x-reg canonical = leader PTX regs`，而 call 边界交换格式固定为完整 `x[256]`；
- structured divergence 期间允许按路径拥有 path-local leader；在每个 `join` 前驱边尾部广播完整 `x[256]`，`join` 后重新选 owner；
- 第一版所有标量指令均按 leader-only 执行，不采用 all-lane 冗余执行主线；
- 整个方案依赖编译器层保证“不存在需要 merge 的 `x-reg` 分裂语义”。

这份设计稿的作用是为后续实现提供统一约束；实现时可以优化物理布局，但不应改变本文定义的语义边界。
