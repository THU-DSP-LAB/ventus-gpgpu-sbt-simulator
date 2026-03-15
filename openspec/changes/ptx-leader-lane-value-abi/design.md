## Context

当前仓库主线实现中，problems 1 / 3 / 4 仍然耦合在一起：

- `sbt/ptx_emit.cpp` 仍把标量 `x-reg` 真值放在 per-warp shared `WarpCtx` 中，标量读写依赖 `ld.shared/st.shared`，leader-only 写后常伴随 `bar.warp.sync`
- 每个 basic block 入口都会固定发射 `activemask/bfind/setp`，把“leader context”作为一个整体缓存对象管理
- direct call 仍通过 `.local __sbt_vctx[1024]` 对 `%v0..%v255` 做整块 spill / restore，并把 `vctx` 指针传给 callee
- 标量条件分支当前使用 `bra.uni`，其正确性依赖所有 active lanes 都能直接读取一致的标量寄存器值

这套实现能工作，但它把“标量 canonical state”、“leader 选择”、“all-lane 读取标量值”、“call 边界状态交换”混成了一套 shared-memory 驱动的协议，导致 PTX lowering 膨胀严重，也让 direct-call ABI 和 leader 语义难以继续演进。

本 change 的设计目标，是在不引入 silent fallback、也不引入 software SIMT stack 的前提下，重构 problems 1 / 3 / 4 的共同状态模型。本文是该 change 的独立技术设计，实施本 change 时应以本设计和本 change 下的 delta specs 为准；旧的 PTX 设计文档仅作为历史背景参考，不再是实施入口。

阅读前提仅要求：

- `README.md`
- `doc/README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `doc/ventus-isa/vbranch_simtstack.md`
- `sbt/ptx_emit.cpp`

## Goals / Non-Goals

**Goals:**
- 以 `leader_lane` + leader-lane PTX scalar regs 重新定义函数体内的 `x-reg` canonical state
- 去掉“每个 basic block 固定重建 `%r1/%r2/%p0`”的 leader preamble 税
- 用统一 value ABI 取代 `.local vctx` context ABI，统一描述 mutable call state、只读 machine context、只读 runtime env
- 让 structured divergence 的 leader / scalar-state 迁移协议闭合，覆盖 `vbranch` 入口、路径内部、`join` 前驱边与 `join` 后当前 leader 选择
- 保持 leader-only 标量执行作为第一版主线；对 all-lane 消费标量值的 lowering，显式使用 leader-to-all-lane 广播
- 把 all-lane scalar consumer 做成显式 inventory，并要求这些路径统一经由 broadcast helper，而不是分散地各自推断
- 把 shared-join / nested-join 的边界与 fail-fast 条件写成显式 gate，而不是只靠实现阶段口头遵守
- 明确区分“语义约束”和“第一版保守物理实现”，避免后续优化被误解成改语义

**Non-Goals:**
- 不处理地址空间专门化问题；普通 `lw/sw/lb/lh/...` 的地址分类优化不属于本 change
- 不引入 all-lane 冗余执行标量作为第一版通用策略
- 不支持非 direct call / 间接调用 / 递归调用
- 不解决超出当前编译器 uniformity contract 的 `x-reg` 路径分裂/语义 merge 问题
- 不把旧 PTX 设计文档继续作为新实现的规范来源

## Decisions

### 1. 用 `leader_lane` 作为唯一持久 leader metadata

新的主线状态模型只保留一个持久 leader 身份字段：`leader_lane`。

其含义是：

- 它标识当前 canonical `x-state` 的拥有者
- 它属于 mutable execution state，而不是 convenience cache
- 它不等价于“当前 active mask 的第一个 lane”

与之对应：

- `activemask` 是 use-point 派生值，不是长期缓存对象
- leader predicate 是 use-point 派生值，即 `laneid == leader_lane`
- `%r1/%r2/%p0` 不再被视为一个必须跨 block 维持的整体 “leader ctx”

这样做的原因是：

- 当前 block preamble 的固定税与大量纯向量 block 无关
- `bfind(activemask)` 在 structured divergence 与新 call ABI 下不再等同于 canonical leader
- 只保留 `leader_lane` 这个持久字段后，call / divergence / join 的状态语义更容易统一

**Alternatives considered:**
- 继续把 `%r1/%r2/%p0` 当作整体 leader context 复用：拒绝。它把 convenience cache 混成了语义状态，也与新的 leader-lane `x-reg` canonical 方案冲突。
- 继续用 `bfind(activemask)` 代表 leader：拒绝。路径局部 leader 与 active-mask 首 lane 不再是同一个概念。

### 2. 函数体内的 canonical `x-reg` 采用 leader-lane PTX scalar regs

函数体执行期间，Ventus `x-reg` 的 canonical state 不再是 shared `WarpCtx.x[]`，而是当前 `leader_lane` 持有的 PTX scalar register state。

该决策包含三条具体规则：

- leader-only 标量 ALU、标量 load/store、CSR 更新等路径仍由 leader-only 执行
- `x0` 仍保持 hard-wired zero
- 非 leader lanes 不承担 canonical `x-reg` 真值的职责

这使得标量 ALU 链可以自然留在 PTX scalar regs 中，而不是每条指令都重新往返 shared memory。

这里依赖当前已经接受的编译器契约：

- 若某个值跨过 `join` 后仍以 `x-reg` 语义存活，则它应当已经是 warp-uniform
- 需要跨 `join` 携带 lane-varying 语义的值，不继续作为标量 live-out `x-reg` 状态存在

第一版允许对违反该契约的输入 fail-fast，而不是引入静默降级。

**Alternatives considered:**
- 继续以 shared `WarpCtx` 作为 canonical `x-reg` backing：拒绝。它是当前 PTX 膨胀的直接来源。
- 第一版直接改成 all-lane 冗余执行全部标量：拒绝。副作用指令、memory store、atomic、CSR 与 call 会立即把问题扩大成整个标量执行模型重写。

### 3. 用 “语义要求 + 第一版保守实现” 定义 structured divergence 协议

structured divergence 的规范性语义要求只有两条：

- 每条实际执行的路径在路径入口时，都必须拥有一个属于该路径 active subset 的 path-local leader，以及与 branch-entry 逻辑 `x-state` 等价的路径入口 scalar state
- 每个 `join` 之后，reconverged active subset 上必须只剩下一份统一的 live scalar state 和一个新的当前 leader；`join` 不是语义 merge 点

基于这两条语义要求，第一版采用以下保守实现策略：

1. 在 `vbranch` 刚发生时，先根据当前 active subset 和 branch 结果得到各路径 mask
2. 对每条非空路径，先选出一个 path-local leader
3. 在路径开始执行前，把 branch-entry 的逻辑 `x-state` 交给该路径的 path-local leader
4. 路径内部继续按 leader-only 标量执行
5. 在 `join` 的每条前驱边尾部，把该路径的 path-final scalar state 重新一致化到该路径当前 active subset
6. `join` 后从 reconverged active subset 中重新选出一个新的当前 leader

第一版的“状态交接/一致化”物理实现选择如下：

- 以“完整 logical `x[256]` 传递”作为保守 baseline
- `vbranch` 入口 handoff 与 `join` 前驱边一致化都允许先用完整 `x[256]` 实现
- 路径 leader 的选取策略，第一版可简单选为对应路径 mask 中的某个固定 lane（例如 first-active lane）

这里要强调层次：

- “路径入口必须合法 leader + 合法 branch-entry `x-state`”是语义要求
- “完整 `x[256]` 传递”、“first-active lane 作为 path-local leader”只是第一版物理实现选择

后续若把完整 `x[256]` 优化成 live subset、dirty subset、或其它摘要化协议，只要语义要求保持不变，就属于实现优化而不是设计推翻。

关于 “leader 选谁” 本身，第一版不需要额外发明复杂策略：

- 当旧 leader 仍合法时，应按当前方案继续继承该 leader 身份
- 当旧 leader 已失效、必须在当前 active subset 内重选时，新 leader 可以是当前 active subset 内任意一个 lane
- 第一版完全可以用 `bfind(activemask)` 作为“如何选出一个合法新 leader”的实现策略

需要避免的不是 `bfind` 这个选举手段本身，而是把 “leader 的语义定义” 重新退化成 “永远等于当前 active mask 的首 lane”。

零 mask 路径的处理规则也在第一版明确：

- 对空路径不要求 leader 选择与 state handoff
- 只对实际执行的非空路径做协议动作

嵌套 divergence 也不引入运行时 path-context 栈；内层 `join` 完成后，外层路径只保留“当前已经一致的 scalar state + 一个新的当前 leader”。

这里需要再明确一条与 Ventus `join` 语义直接相关的约束：

- 方案必须覆盖 shared-join 形态，即同一条 `join` 指令在嵌套结构里可能被连续命中多次
- 每次真正抵达某个 `join` 前驱边时，都只对“当前这一层实际完成 reconverge 的路径状态”执行 predecessor-edge 协议
- 内层 `join` 完成后留下的是新的当前 leader 与新的当前一致 `x-state`，而不是恢复某个历史 leader 身份

换句话说，本设计虽然不做 lane-wise `x-state merge`，但也不允许把 shared-join / nested-join 情况简化成“每个 join 只会命中一次”的普通 if/else 心智模型。

此外，第一版虽然接受 uniformity contract，但不能只把它当成注释性前提。实现必须把下面这些情况视为 fail-fast gate：

- 无法证明某个跨 `join` 继续作为 `x-reg` 读取的值满足当前 uniformity contract
- 无法可靠识别某个 structured `join` 的前驱边集合与插入位置
- shared-join / nested-join 形态下，无法确定 predecessor-edge 协议与 `join` 后 leader 重选的放置点

第一版不要求做完美静态分析；但不能在这些边界不清时继续生成“看起来能跑”的 PTX。

**Alternatives considered:**
- 只定义 `join` 前驱边协议，不定义 `vbranch` 入口 handoff：拒绝。这样当原 leader 不在某条路径中时，路径一开始就没有合法 scalar state。
- 把 `join` 当作需要 lane-wise merge 的标量 PHI 点：拒绝。与当前编译器契约冲突，也会让实现复杂度失控。
- 在设计层冻结某一种唯一物理同步方案：拒绝。后续明显还需要对 PTX 体积做优化。

### 4. direct call 统一迁移到 value ABI，但第一版仍保守保持 leader-lane scalar 语义

新的 direct call 设计分三层：

- `Mutable CallState`
- `ReadOnly MachineContext`
- `ReadOnly RuntimeEnv`

其中：

- `Mutable CallState` 至少包含完整 logical `x-reg`、完整 logical `v-reg`、所需可变 CSR，以及 `leader_lane`
- `ReadOnly MachineContext` 包含当前 helper 需要读取、但对一次 kernel 执行视为只读的机器上下文
- `ReadOnly RuntimeEnv` 包含 ELF/global backing、heap backing 等宿主运行时环境字段

新的逻辑调用接口是：

- 输入：`Mutable CallState` + `ReadOnly MachineContext` + `ReadOnly RuntimeEnv`
- 输出：调用后的 `Mutable CallState`

第一版的物理实现不要求单一 mega-blob。允许按少量聚合对象分段，只要逻辑分层不变即可。

在 leader-lane scalar-state 路径下，call 语义还有两条第一版策略：

- ordinary call / ret 不主动重新发明新的 leader 选择规则；当前 executing path 的 leader-lane scalar 语义在 call 前后保持连续
- 即使第一版大多数情况下 call 前后 leader 不变，`leader_lane` 仍然必须显式进入 call ABI，并且 caller 在 return 后必须把返回的 `leader_lane` 视为 authoritative

这里还需要补一条顺序约束：

- direct call 本身不属于 “all-lane scalar consumer”，因此不要求像 `vx` / `bra.uni` 那样先把 scalar 源广播成 all-lane 局部值
- 但 call ABI 的 scalar marshalling / unmarshalling 必须以当前 leader 所持有的 canonical `x-state` 为 authoritative 来源
- callee 在执行任何 Ventus 标量相关操作之前，必须先从 call-state 恢复 `leader_lane` 与 canonical scalar state；不能先做 leader-only Ventus 标量初始化，再回头覆盖 leader/state

这样做的原因是：

- 语义上避免把 leader 变化建立在“隐含约定永远不变”上
- 工程上允许后续在不重写整个逻辑 ABI 的前提下调整 physical layout 或 leader 处理策略

在 structured divergence 内发生 direct call 时，callee 消费的是当前路径的 path-local mutable state，而不是 warp 入口的历史 leader/state。

helper prototype 也必须跟随这套 ABI 同步演进：

- prototype / definition 都必须反映新的 value ABI 参数分层
- 不能保留旧的 `elf_base ... vctx_base` 固定参数列作为主线 prototype 语义
- prototype emission 的作用仍然只是消除前向调用顺序依赖，而不是继续绑定旧 ABI

**Alternatives considered:**
- 保留 `.local vctx` context ABI：拒绝。即使 helper 只用很少状态，也要固定承担整块 spill/restore 成本。
- 采用大量离散 `.param .b32` 的 `value_params`：拒绝。实验已经表明它的接口宽度和文本膨胀都更差，不适合作为主线。
- 不把 `leader_lane` 放入 call ABI，只靠 callee 自己从 `activemask` 猜：拒绝。这样会把 leader 语义重新退化成旧模型。

### 5. implicit exec state 明确不进入 value ABI

新的 value ABI 只显式承载：

- mutable call state
- read-only machine context
- read-only runtime environment

以下执行态明确不进入 value ABI：

- `active mask`
- PTX / 硬件隐式维持的收敛 / 发散执行上下文
- 任何由当前执行子集派生出的 convenience 状态

原因是这些值已经由 PTX / 硬件执行模型隐式维持；若再把它们序列化进 blob，会出现“显式字段”和“真实执行态”两份真值来源。

这条排除规则是规范性要求，不是实现建议。

**Alternatives considered:**
- 把 `activemask` 或类似执行态字段一起塞进 call state：拒绝。它会重新引入双真值来源，并使 call ABI 与真实执行态脱节。

### 6. 所有 all-lane 消费标量值的 lowering，统一使用 leader-to-all-lane broadcast

第一版仍坚持 leader-only 标量执行，但必须显式区分：

- leader-only 标量 use
- all-lane 需要读取统一标量值的 use

后者统一通过 leader-to-all-lane broadcast 实现，不能再假设所有 active lanes 本地已经持有 canonical scalar value。

当前已经确认必须这样处理的场景至少包括：

- 标量条件分支 `beq/bne/blt/bge/bltu/bgeu`
- 任何需要所有 active lanes 消费统一标量源的 `vx` / 类似 lowering

为了避免“原则正确但漏修 use-site”，实现与验证必须从当前 emitter 反推出一份显式 inventory，并至少覆盖当前已知的这几类路径：

- 标量条件分支
- `vmv_v_x` / `vmv_s_x` / `vfmv_v_f`
- `vmerge_vxm` / `vfmerge_vfm`
- `vadd_vx` 及其它 `vx` 风格向量算术或混合消费路径

该 inventory 的作用不是冻结最终实现细节，而是提供一张可核对的闭表，确保实现者不会只修 branch 而遗漏其它 all-lane scalar consumer。

对于标量条件分支，第一版明确采用下面的策略：

1. 从当前 leader lane 广播参与比较的标量源操作数到 active subset
2. 所有 active lanes 基于广播结果计算同一个 branch predicate
3. 继续使用现有 `bra.uni` 结构化 lowering

这是一条局部但必要的修补：

- 它修复了 leader-lane canonical `x-reg` 方案下，标量分支不能再直接从非 leader 本地 scalar regs 取值的问题
- 它不要求把所有标量指令都改成 all-lane 冗余执行

设计与任务中必须显式列出所有 all-lane scalar consumers，防止实现者只修 branch 而遗漏其他 use site。

进一步说，第一版应尽量把这些 use-site 都收敛到单一的 broadcast helper 入口，而不是允许每个 lowering 私自决定如何“顺便拿到”统一 scalar 值。否则 inventory 即使存在，也仍会在实现阶段退化成难以审计的分散逻辑。

**Alternatives considered:**
- 只修标量分支，不建立“all-lane scalar consumer 必须 broadcast”的统一规则：拒绝。会留下同类漏网路径。
- 直接改成 all-lane 冗余执行所有标量：拒绝。它会把 branch 修补扩大成整套执行模型重写。

### 7. block 入口不再承担 leader materialization 责任；derived state 只在 use 点生成

新的 emitter 组织方式中，basic block 仍然是 CFG / label / branch target 单元，但不再负责自动准备 `%r1/%r2/%p0`。

新的 lowering primitive 至少需要明确拆成：

- `emit_read_activemask(...)`
- `emit_make_is_leader_pred(...)`
- `emit_broadcast_leader_scalar_to_active(...)` 或等价 helper

call / divergence / join 只会使 derived state 失效，不会抹掉 `leader_lane` 本身。任何跨这些边界的 cached mask/predicate 都必须视为 stale 并重新派生。

`bar.warp.sync` 也不再被视为 leader 判定机制的一部分。后续如果某个具体状态交换协议仍需要同步，它只是该协议的局部实现需求。

**Alternatives considered:**
- 继续按 block 边界维护 `leader_ctx_in_valid/out_valid` 状态机：拒绝。语义上已经没有必要，且实现复杂度高。

### 8. 迁移按 staged coexistence 进行，但不允许 silent fallback

从当前代码过渡到新方案时，短期内可能出现旧路径与新路径并存：

- 旧 shared `WarpCtx` / `wctx_ptr` 路径
- 旧 `.local vctx` 路径
- 新 leader-lane scalar-state / value ABI 路径

迁移策略要求：

- 任何仍存活的旧路径，只要它的正确性还依赖 shared state 或同步，就必须保持这些动作，直到该路径被完全替换
- 新路径不能通过静默 fallback 回退到旧 shared model 来“先跑起来”
- 文档和代码注释必须明确哪些是“当前保守第一版选择”，哪些是“后续可替换的物理实现”

这既符合项目当前的 fail-fast / no silent fallback 原则，也能降低迁移阶段的调试成本。

**Alternatives considered:**
- 在新路径不完整时自动回退到旧 shared 或 `vctx` 模式：拒绝。会把语义缺口隐藏起来，后续更难定位错误。

## Risks / Trade-offs

- 第一版用完整 logical `x[256]` 做 `vbranch` 入口 handoff、`join` 前驱一致化、以及 call 边界 state exchange，正确性最容易说明，但 PTX 体积和 `ptxas` 压力未必最优。这是有意识接受的保守起点。
- `value ABI` 相比 `vctx` 的收益，在真实 kernel 上大概率成立，但仍然受 mutable state 规模和 `ptxas` 决策影响；高压区可能出现 spill / lmem 上升。
- 这套设计最容易出功能错误的地方，是遗漏某个 all-lane scalar consumer 没有先做 broadcast，或者在 call / divergence / join 后错误复用 stale 的 derived mask/predicate。
- shared-join / nested-join 是另一个高风险区：若 predecessor-edge 协议、leader 重选或 fail-fast gate 定义不清，很容易得到“普通分支能跑、复杂结构错”的隐蔽错误。
- 迁移阶段旧新路径并存时，最危险的误操作是“因为新设计里 leader 不依赖 sync，就直接删掉旧 shared path 上仍然必需的同步”。
- 本设计依赖当前接受的编译器 uniformity contract。如果未来输入范围扩大到该契约无法保证，需要重新评估 leader-lane scalar-state 方案，而不是靠隐式补丁修修补补。
