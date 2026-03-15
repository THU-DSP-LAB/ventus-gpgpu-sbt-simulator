# PTX Owner-Lane Derivation 设计

本文是 `doc/PTX_LOWERING_REDUCTION_PLAN.md` 中“问题 3：leader 上下文当前按基本块固定重建”的正式设计稿。目标读者是未参与过本项目的维护者；只要已阅读：

- `doc/IMPLEMENTATION_CODEMAP.md`
- `doc/PTX_LOWERING_REDUCTION_PLAN.md`
- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`
- `sbt/ptx_emit.cpp`

即可依据本文理解并实施新的方案。

本文讨论的是 **PTX emitter 中 owner / active-mask / owner predicate 的管理方式**。它与 `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md` 中的 direct-call `value_blob` ABI 设计强耦合：若后者的 `x-reg canonical = leader PTX regs` 与 structured divergence 协议发生变化，本文也必须同步调整。

## 1. 设计目标

当前 emitter 在每个 basic block 入口固定发射：

```ptx
activemask.b32 %r1;
bfind.u32 %r2, %r1;
setp.eq.u32 %p0, %r0, %r2;
```

这带来的问题是：

- 纯向量 block 也无条件支付固定税；
- 只有简单跳转的 block 也支付固定税；
- `%r2/%p0` 当前建立在“active-lane leader = bfind(activemask)”的旧语义上，不再兼容新的 `value_blob` / `leader_lane owner` 设计。

本文的新目标是：

1. 去掉“每个 basic block 入口必须重建 `%r1/%r2/%p0`”的规则；
2. 使问题 3 与新的 `value_blob` ABI 相容；
3. 明确区分：
   - 跨 call / structured divergence 持续存在的 **owner metadata**
   - 由当前执行状态临时派生的 **mask / predicate**
4. 在不引入 silent fallback 的前提下，避免旧方案中的框架 PTX 固定税。

## 2. 结论先行

新方案的核心结论只有三条：

1. **持久状态只保留 `owner_lane`**
   - 它是 `x-state` 的 canonical owner 身份；
   - 它属于 mutable state；
   - 它由 direct-call `value_blob` ABI 与 structured divergence 协议维护。

2. **`active mask` 不缓存，按 use 点现取**
   - 需要当前 active subset 时，直接发 `activemask`;
   - 不再维护 `%r1` 作为长期语义状态；
   - `active mask` 也不进入 blob。

3. **`is_owner` 不缓存，按 use 点现算**
   - 需要 leader-only predicate 时，比较 `laneid == owner_lane`;
   - 不再维护旧语义下的 `%p0` 作为长期语义状态；
   - 也不再保留“`%r2 = bfind(activemask)` 是当前 leader lane”这一旧概念。

可用下面的最小模型概括：

```text
persistent state:
  owner_lane

derived on demand:
  cur_mask   = activemask()
  is_owner   = (laneid == owner_lane)
```

## 3. 与旧方案相比，什么被放弃了

本文明确放弃旧设计中的三个概念：

### 3.1 放弃“`leader ctx = {%r1,%r2,%p0}` 是一个整体缓存”

旧方案把 `%r1/%r2/%p0` 看成一个一起 materialize、一起 kill、一起复用的整体。这在新的 `value_blob` 设计下不再成立。

原因：

- `owner_lane` 不再等于 `bfind(activemask)`；
- `active mask` 不显式进入 call ABI；
- `%p0` 只是基于 `owner_lane` 的局部派生谓词，不再是跨区域缓存对象。

### 3.2 放弃“按 basic block 边界管理 leader context”

basic block 仍然保留，但只作为：

- CFG 单元；
- label / branch target 单元；
- 代码发射单元。

它不再承担“进入这里就必须准备好 `%r1/%r2/%p0`”的职责。

### 3.3 放弃“`bar.warp.sync` 是 leader 机制的固定组成部分”

旧实现中的很多 `bar.warp.sync %r1` 来自：

- 标量 `x-reg` 真值驻留在 per-warp shared `WarpCtx`；
- leader-only 写 shared 状态后，需要对齐其它 active lanes。

在新设计中，`bar.warp.sync` 不再是 owner 判定或 owner predicate 的固定组成部分。

若某个具体状态交换协议仍需要 `bar.warp.sync`，它应被视为：

- 某段协议实现的局部需求；
- 而不是“leader 机制天然必须附带”的通用步骤。

## 4. 新方案的状态分层

### 4.1 持久状态：`owner_lane`

`owner_lane` 表示当前 `x-state` 的 canonical owner 身份。

它的来源与维护不由问题 3 单独决定，而是依赖 `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md` 已冻结的约束：

- direct call 边界通过 `Mutable CallState` 显式传递 `leader_lane`
- structured divergence 中允许 path-local owner
- 每个 `join` 的前驱边尾部广播完整 `x[256]`
- `join` 后重新选择 owner

因此，在问题 3 的文档里，`owner_lane` 不是“推导对象”，而是**输入前提**。

### 4.2 派生状态：`active mask`

当前 active subset 通过 PTX `activemask` 指令读取。

这有两个直接含义：

- `active mask` 不进入 blob；
- emitter 不需要维护“长期有效的 `%r1`”这一语义对象。

若某个 lowering 需要 mask：

- 就在 use 点现发 `activemask.b32 tmp;`

而不是假设 block 入口已经准备好 `%r1`。

### 4.3 派生状态：`is_owner`

需要 owner-only 执行时，在 use 点现算：

```ptx
setp.eq.u32 %pX, %laneid, owner_lane;
```

这里的关键是：

- 比较对象是 `owner_lane`
- 不是 `bfind(activemask)` 的结果

所以旧语义下的 `%r2 = active-lane leader` 必须退出设计主线。

## 5. 功能正确性的前提与检查

本节只讨论与问题 3 直接相关、足以导致功能性错误的前提。

### 5.1 前提 1：在任何 owner-only use 点，`owner_lane` 必须是当前 active subset 的成员

如果在某个 use 点：

- `owner_lane` 不 active
- 但 lowering 仍直接用 `laneid == owner_lane` 作为 owner predicate

则不会有任何 active lane 执行该 owner-only 操作，属于直接功能错误。

因此，本设计依赖 `value_blob` 文档中的协议保证这一点：

- 进入 divergence path 后，会选择 path-local owner；
- 在 `join` 前驱边尾部，会先把完整 `x[256]` 广播给该路径 active subset；
- `join` 后重新选 owner；
- direct call 返回的 `leader_lane` 与 `x-state` 共同定义新的 owner。

结论：

- **问题 3 自身不负责修复 inactive owner**
- 它只依赖上层协议保证：任何 owner-only use 点之前，当前 `owner_lane` 已合法

### 5.2 前提 2：所有 owner-only lowering 必须改用 `owner_lane`，不能继续依赖 `bfind(activemask)`

若实现仍有路径沿用旧逻辑：

```ptx
activemask -> bfind -> compare laneid with bfind result
```

则它得到的是“当前 active subset 的第一个 lane”，而不是 `x-state` owner。

在新方案下，两者不再等价。

这也是最容易引入隐蔽功能性错误的地方之一。

### 5.3 前提 3：`call` 返回后不能继续沿用 call 前派生出的 owner predicate

在新 `value_blob` ABI 下：

- callee 可能修改 `owner_lane`
- callee 返回时会带回新的 `Mutable CallState`

因此 call 返回后：

- 旧的 `is_owner`
- 旧的“当前 owner 就是某个 lane”的任何本地推导结果

都必须视为失效，必须按新的 `owner_lane` 重新推导。

### 5.4 前提 4：旧的 `bar.warp.sync` 不能被误删到破坏当前旧实现路径

本文描述的是**新语义方案**。

但仓库当前主线仍然存在：

- per-warp shared `WarpCtx`
- leader-only 写 shared 状态
- 写后 `bar.warp.sync`

因此在迁移实现时：

- 只有当相关路径已切换到新语义协议，才能去掉“为 shared 可见性服务”的 `bar.warp.sync`
- 不能因为“新设计里 owner 判定不依赖 sync”就把旧共享状态路径上的同步直接全部删除

这是迁移阶段的功能风险，不是最终设计本身的语义风险。

## 6. semantic kill / use 的新定义

### 6.1 semantic kill

在新方案里，需要区分两种东西：

- `owner_lane`
- 派生结果（mask / predicate）

`vbranch` 与 `join` 仍然是与 active subset 变化相关的关键语义边界，但它们不再意味着“整个 leader ctx 被 kill”。

更准确的说法是：

- 它们会使**旧的派生结果**失效；
- 但不会直接抹掉 `owner_lane` 这一持久状态；
- 具体的 owner 迁移由 `value_blob` 设计中的 divergence / `join` 协议负责。

因此，旧文档里的“semantic kill = 整体 leader ctx invalid”表述必须废弃。

### 6.2 use

在新方案里，use 的判定标准变成：

- 任何需要读取当前 active subset 的 lowering，都是 mask use；
- 任何需要 owner-only 执行的 lowering，都是 owner-predicate use。

可以写成两类 primitive：

#### mask use

- 需要当前 active mask 的场景
- 典型代表：若某条协议仍需 `bar.warp.sync <mask>`，则它是 mask use

#### owner-predicate use

- 需要生成 `laneid == owner_lane` 的谓词
- 典型代表：leader-only 标量 ALU / 标量访存 / owner-only x-reg 更新

注意：

- use 仍然按 lowering primitive 判定
- 不按 Ventus 指令大类硬编码
- 当前版本的标量分支 `beq/bne/blt/bge/bltu/bgeu` 仍不属于 owner-predicate use，因为它们按 `bra.uni` lowering，不消费 owner predicate

## 7. PTX emitter 的实现建议

### 7.1 删除固定 preamble

`emit_body()` 不应再在每个 basic block 入口无条件发射：

```ptx
activemask.b32 %r1;
bfind.u32 %r2, %r1;
setp.eq.u32 %p0, %r0, %r2;
```

对应旧 helper `emit_block_preamble()` 应退出主线。

### 7.2 用显式 helper 代替旧 `%r1/%r2/%p0` 全家桶

建议引入两个语义明确的 helper 类别：

1. `emit_read_activemask(dst_r)`
   - 只负责在 use 点读取当前 active mask

2. `emit_make_is_owner_pred(dst_p, owner_r)`
   - 只负责基于 `owner_lane` 生成 owner predicate

如果某个 lowering 同时需要：

- 当前 active mask
- owner predicate

则显式先后调用二者，而不是隐式依赖某个整体 “leader ctx 已准备好”。

### 7.3 `owner_lane` 的来源

问题 3 本身不规定 `owner_lane` 在具体 PTX 中存放于哪个寄存器编号；但要求 emitter 有一个清晰、单一的“当前 owner lane 值来源”。

该来源在不同场景下可能来自：

- `.entry` 初始值
- `join` 后新选择出的 owner
- direct call 返回的 `Mutable CallState.leader_lane`

但不应来自“现算 `bfind(activemask)`”。

### 7.4 block 边界上的处理方式

basic block 仍然保留，但在本方案里：

- block 入口不再承担任何固定 materialization 责任；
- 也不需要维护旧文档里的 `leader_ctx_in_valid / leader_ctx_out_valid` 布尔状态；
- 发射逻辑只需保证：
  - 当 lowering 需要 mask 时，现取 `activemask`
  - 当 lowering 需要 owner predicate 时，现算 `laneid == owner_lane`

因此，问题 3 的实现复杂度会明显低于旧的“跨 block leader ctx 缓存 + kill/use 状态机”方案。

## 8. `bar.warp.sync` 的新地位

### 8.1 不再是 owner 判定机制的一部分

在新方案里，`bar.warp.sync` 不再用于：

- 选择 owner
- 重建 owner predicate
- 保持 owner metadata 的正确性

这些逻辑都不需要它。

### 8.2 只在特定状态交换协议需要时出现

若未来某段协议仍然需要 warp 内同步，例如：

- structured divergence 边尾部的 `x[256]` 广播实现
- 某段共享 scratch 协议

则可以在该协议局部发 `bar.warp.sync`。

但这种同步必须被文档明确解释为：

- “该状态交换协议的实现需求”

而不是：

- “leader 机制总是必须附带的固定模板”

## 9. 与 `value_blob` 设计的兼容关系

本文方案与 `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md` 的兼容点如下：

1. `owner_lane` 与其 `leader_lane` 字段一一对应；
2. `active mask` 不入 blob，而是按 use 点 `activemask` 读取；
3. `join` 前驱边尾部完整 `x[256]` 广播负责保证 owner-only use 点前 owner 合法；
4. direct call 返回后重新使用 call 返回的 `leader_lane`；
5. 不再把 “leader = bfind(activemask)” 作为通用心智模型。

若未来 `value_blob` 文档对以下内容发生变化，本文必须同步更新：

- `x-reg canonical = leader PTX regs`
- `join` 前驱边尾部完整 `x[256]` 广播
- direct call 返回的 `leader_lane` 语义

## 10. 非目标

本设计不负责：

- 定义 `owner_lane` 在 structured divergence 中的完整迁移协议
- 定义 `x[256]` 广播的具体 PTX 物理实现
- 规定 direct-call `mutable_state_blob` 的物理布局
- 规定 `bar.warp.sync` 是否在某个具体广播协议中出现

这些内容由：

- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`
- 以及后续具体实现设计

共同定义。

## 11. 实施检查清单

实现时建议至少核对以下事项：

1. 删除每个 block 入口的固定 `emit_block_preamble()`
2. 删除“长期 `%r1/%r2/%p0`”这一设计假设
3. 所有 owner-only lowering 改为基于 `owner_lane` 现算 predicate
4. 所有需要当前 active subset 的 lowering 改为 use 点现取 `activemask`
5. 标量分支 `bra.uni` 路径保持不依赖 owner predicate
6. call 返回后不复用 call 前局部推导出的 owner predicate
7. 迁移阶段不得误删旧 shared-state 路径真实需要的 `bar.warp.sync`

## 12. 验证建议

落地后建议至少验证：

### 12.1 PTX 文本收益

对若干现有 kernel 比较修改前后：

- `activemask.b32` 次数
- `bfind.u32` 次数
- `setp.eq.u32 %p0, %r0, %r2` 次数

预期应显著下降，尤其是：

- 纯向量 block
- 纯跳转 block
- 旧方案里仅为 block preamble 支付固定税的区域

### 12.2 call / divergence 功能正确性

重点验证：

- direct call 返回后 owner-only 标量路径仍正确
- divergence path 内 direct call 使用 path-local owner 时仍正确
- `join` 后重新选 owner 的路径仍正确

### 12.3 迁移阶段兼容性

若实现与旧 `WarpCtx` / `wctx_ptr` 路径共存，还需验证：

- 尚未迁移的旧路径不会因为删除固定 `%r1/%r2/%p0` preamble 而失效
- 尚未迁移的 shared-state 协议不会因误删 `bar.warp.sync` 而失效

## 13. 结论

问题 3 的新方案不再是“leader context lazy materialization / 跨 block 复用”问题，而是：

- **只保留 `owner_lane` 作为持久状态**
- **在 use 点现取 `activemask`**
- **在 use 点现算 `is_owner`**
- **彻底移除 `%r2 = bfind(activemask)` 这一旧 leader 语义**

它比旧方案更简单，也与新的 `value_blob` / `x-reg canonical = leader PTX regs` 设计一致。若后续实现需要进一步优化 PTX 条数，可以在这一正确语义基础上再评估是否值得重新引入局部缓存；但那应是性能优化，不再是基础语义方案。
