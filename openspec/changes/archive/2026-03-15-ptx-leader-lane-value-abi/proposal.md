## Why

当前 PTX emitter 的主线实现仍有三类彼此耦合的问题：

- 标量 `x-reg` 仍以 per-warp shared `WarpCtx` 作为真值来源，导致大量 `ld.shared/st.shared` 与 `bar.warp.sync`
- basic block 入口固定重建 `activemask/bfind/is_leader`，即使 block 内没有 leader-only 标量操作也要支付固定 PTX 成本
- direct call 仍通过 `.local vctx` 对 `%v0..%v255` 做整块 spill / restore，调用边界成本过高

这三类问题不能再作为独立的局部优化处理。只要 `x-reg` 仍驻留 shared `WarpCtx`，leader 上下文就会继续依赖旧的 `%r1/%r2/%p0` 语义；只要 direct call 仍走 `vctx` context ABI，`x-reg`、`v-reg` 与可变 CSR 也无法收敛到统一的状态交换模型。

在前期方案审查中，已经进一步明确了两点实现边界：

- structured divergence 不能只定义 `join` 前的状态交换，还必须定义 `vbranch` 刚发生时如何把 branch-entry 的 `x-state` 交给每条路径的 path-local leader
- 标量条件分支 `beq/bne/blt/bge/bltu/bgeu` 继续沿用 `bra.uni` 是可行的，但在 leader-lane canonical `x-reg` 方案下，必须先把参与比较的标量源从 leader lane 广播到所有 active lanes

进一步审查后，又确认了两个必须纳入正式 change 的高风险边界：

- `all-lane scalar consumer` 不能只靠“实现时记得修”来覆盖；必须形成一份从当前 emitter 反推出的 use-site 闭表，并要求这些路径统一经由显式 broadcast helper 消费标量值
- `join` 不做 `x-state` 语义 merge 的前提虽然成立，但必须把 shared-join / nested-join 形态与结构协议 fail-fast 边界一起写清；post-`join` 标量 uniformity 仍主要依赖编译器 contract，而不是在本 change 中补静态证明

因此，需要一个新的、可独立阅读和实施的 OpenSpec change，把 leader-lane 标量 canonical state、direct-call value ABI、leader/use-point derivation、以及标量分支广播修补统一纳入一份自洽方案。该 change 后续产生的 specs/design 应成为实施本改进的主入口，而不是继续依赖旧的 PTX 设计文档拼接背景。

## What Changes

本 change 将定义并收敛以下改进方向：

- 将函数体内的标量 `x-reg` canonical state 从 shared `WarpCtx` 迁移到 leader lane 的 PTX 标量寄存器
- 用 `leader_lane` 作为唯一持久 leader metadata，在 use 点按需派生 `activemask` 与 leader predicate，移除“每个 basic block 固定重建 leader ctx”的规则
- 将 direct call 从 `.local vctx` context ABI 迁移到聚合 `value_blob` 风格的 value ABI，统一传递 mutable call state、只读 machine context 与只读 runtime env
- 定义 structured divergence 下的 leader 迁移协议，既覆盖 `vbranch` 入口时的 branch-entry `x-state` 交接，也覆盖 `join` 前驱边尾部的路径状态一致化
- 保留 leader-only 标量执行作为第一版主线，对所有 all-lane 消费标量值的 lowering 明确采用 leader-to-all-lane 广播；其中标量条件分支必须在 `bra.uni` 前完成源操作数广播
- 把当前 emitter 中的 all-lane scalar consumer 做成显式 inventory，并要求这些路径统一走 broadcast 入口，而不是继续直接读取“本地已一致”的标量值
- 把 shared-join / nested-join 场景下的 leader / scalar-state 协议、以及 predecessor 放置无法可靠确定时的 fail-fast 规则，写成规范性要求
- 在 OpenSpec artifacts 中完整写清当前选择的语义边界、第一版物理实现选择、为何暂不引入 all-lane 标量执行，以及哪些点是未来可以替换的实现策略

本 change 不包含地址空间专门化问题；普通 `lw/sw/lb/lh/...` 的地址分类优化不在本次范围内。

## Capabilities

### New Capabilities

- `leader-lane-scalar-state`
  - PTX emitter 以 leader lane PTX 标量寄存器作为 `x-reg` 的函数体 canonical state
  - structured divergence 必须定义 `vbranch` 入口与 `join` 前驱边的 leader / `x-state` 交接协议
  - direct call 边界必须能够显式携带 `x-reg` 与 `leader_lane`

- `direct-call-value-abi`
  - direct call 使用聚合 value ABI 传递统一的 mutable call state，而不是 `.local vctx` 整块 spill / restore
  - mutable state 至少覆盖 `x-reg`、`v-reg`、可变 CSR 与 `leader_lane`
  - 只读 machine context 与 runtime env 必须与 mutable state 分层

- `leader-broadcast-all-lane-uses`
  - 保持 leader-only 标量执行主线
  - 对所有需要全体 active lanes 观察统一标量值的 lowering，明确采用 leader-to-all-lane 广播
  - 标量条件分支继续使用 `bra.uni`，但必须先广播参与比较的标量源

### Modified Capabilities

- `ptx-emitter-leader-derivation`
  - block 入口不再固定 materialize `activemask/bfind/is_leader`
  - `activemask` 与 leader predicate 改为在 lowering use 点按需生成
  - 旧的“leader = bfind(activemask)”语义退出主线

- `ptx-call-prototype`
  - helper prototype 与 helper definition 必须同步切换到新的 value ABI 参数分层
  - prototype emission 不能继续固化旧的 `elf_base ... vctx_base` 参数列

## Impact

- 该 change 会重构 `sbt/ptx_emit.cpp` 中与标量寄存器、leader 上下文、direct call、以及结构化分歧相关的核心 lowering 路径，属于高影响改动
- 该 change 还会同步影响 helper `.func` prototype 的参数形状；prototype 规格必须与新的 value ABI 一起更新
- 现有 shared `WarpCtx` / `wctx_ptr` / `.local vctx` 路径可能需要经历阶段性共存，但最终主线语义应以 leader-lane scalar state 与 value ABI 为准
- 后续 specs/design 必须写成自包含文档：只要求阅读源码与公共入口文档（如 `README.md`、`doc/README.md`、`doc/IMPLEMENTATION_CODEMAP.md`）即可上手实施，不要求预先掌握旧 PTX 设计文档
- 验证重点将覆盖 direct call、nested `vbranch/join`、`vbranch` 入口 leader 交接、`join` 后 leader 重选、以及标量条件分支的广播后 `bra.uni` 语义正确性
- 验证计划还必须覆盖 shared-join 形态，以及“all-lane scalar consumer inventory 与实际 lowering use-site 一致”的审查与回归
