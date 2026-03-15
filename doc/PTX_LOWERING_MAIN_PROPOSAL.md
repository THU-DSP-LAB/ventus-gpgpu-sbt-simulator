# PTX Lowering 主提案

本文档是当前 PTX lowering 演进工作的**主入口**。
阅读顺序建议如下：

1. 本文
2. `doc/IMPLEMENTATION_CODEMAP.md`
3. `sbt/ptx_emit.cpp`
4. 如需背景，再按需阅读历史设计/分析参考

本文档的目标不是重新展开所有历史讨论细节，而是把当前项目已接受的方案、边界和实施顺序收敛为一份稳定、可长期引用的说明。后续 OpenSpec 变更与实现应以本文作为 problems 1 / 3 / 4 的持久 canonical reference。

## 1. 当前目标

当前阶段的重点不是继续做“更多指令先跑通”，而是提升 **Ventus ELF -> PTX lowering 质量**：

- 缩短生成 PTX 的指令序列；
- 优先去掉与单条 Ventus 指令无关的框架性 PTX 冗余；
- 为后续 `ptxas` / SASS 质量提供更干净的输入。

这里讨论的是 **生成 PTX 的质量**，不是 `sbt_ptx` 翻译器本身的运行速度。

## 2. 当前主问题划分

当前已识别的主要膨胀源有四类：

### 2.1 问题 1：标量 `x-reg` 目前驻留在 per-warp shared `WarpCtx`

当前实现里：

- 标量读写依赖 `ld.shared/st.shared`
- leader-only 标量更新后常带 `bar.warp.sync`
- 标量计算链无法自然留在 PTX 寄存器中

这是当前最大、最系统性的 PTX 膨胀源之一。

### 2.2 问题 2：普通访存大量走通用数值地址映射模板

当前 `lw/sw/lb/lh/...` 和对应向量访存常展开为：

- shared/heap 判定
- pointer 计算
- ELF/heap 覆写
- 再做真实 load/store

这个问题仍然活跃，且**本主提案不试图完全取代它对应的专项规划**。因此 `doc/PTX_LOWERING_REDUCTION_PLAN.md` 仍保留为 active 文档，主要就是因为问题 2 尚未在本文范围内被完全收束。

### 2.3 问题 3：leader context 目前按 basic block 固定重建

当前每个 block 入口都固定发：

```ptx
activemask.b32
bfind.u32
setp.eq.u32
```

即使 block 只包含纯向量算术，也要支付这三条固定税。

### 2.4 问题 4：direct call 目前通过整块 `vctx` spill / restore 维持向量状态

当前 direct call 方案会在 caller/callee 边界对 `%v0..%v255` 做整块 `.local` spill / restore。这对多函数 kernel 会引入大量 `ld.local/st.local` 与样板代码。

## 3. 当前接受的总体方向

当前项目对 problems 1 / 3 / 4 接受的总体方向是：

- 用一套统一状态模型重新解释标量状态、owner metadata 与 direct-call mutable state；
- 把 problem 1 / 3 / 4 视为**耦合问题**而不是三个独立局部优化；
- 在新模型落地后，再回头进一步压缩框架冗余与 call ABI 成本；
- problem 2 继续独立推进，不强行并入这套状态模型重构。

可以概括成下面这张图：

```text
problem 1: x-reg canonical form
        \
         +-- shared state / owner model --+
        /                                 |
problem 3: leader derivation              +-- direct-call mutable ABI
        \                                 |
         +-- owner_lane / use-point derivation --+
        /
problem 4: call state transfer

problem 2: address specialization
  -> independent optimization track
```

## 4. 当前接受的状态模型

### 4.1 `x-reg` 的长期方向

当前接受的方向是：

> `x-reg canonical = leader PTX regs`

也就是说：

- 函数体执行期间，逻辑标量状态的 canonical form 不再是 shared `WarpCtx.x[]`
- canonical owner 由 `owner_lane` 标识
- call 边界交换格式仍按逻辑完整 `x[256]`

### 4.2 `owner_lane` 是持久状态，`activemask/is_owner` 是 use-point 派生值

当前接受的方向是：

- 持久状态保留 `owner_lane`
- `activemask` 不作为长期缓存，也不进 blob
- owner predicate 在 use 点由 `laneid == owner_lane` 现算

因此：

- 旧的 `bfind(activemask)` 领导者语义不是新主线
- 每个 basic block 固定重建 `%r1/%r2/%p0` 也不是新主线

### 4.3 direct call 的长期方向是 `value_blob`

当前接受的方向是：

- 放弃以 `vctx` 为长期主线的 context ABI
- 采用聚合对象 value ABI
- 统一建模 mutable call state、只读 machine context、只读 runtime env

在这条线上，`value_params` 仅保留为历史实验对照，不作为主方案。

## 5. 当前接受的关键前提

本文必须明确区分两类东西：

- 本仓库当前代码已经体现的事实
- 本项目当前接受的上游/实验性信任边界

### 5.1 已知代码事实

当前代码事实包括：

- `x-reg` 仍在 shared `WarpCtx` 中实现
- block 入口仍固定重建 leader ctx
- direct call 仍使用 `vctx` 整块 spill / restore

也就是说，本文描述的是**当前接受的演进方向**，不是说这些内容已经全部实现完成。

### 5.2 当前接受的上游编译器契约

根据 `doc/ventus-divergence-sgpr-analysis.md`，当前项目接受如下编译器契约：

- Ventus LLVM 不把 `vbranch` / `join` 当成 SGPR save/restore 边界
- 会变成 lane-varying 的 SSA 值应走 VGPR 路径
- `join` 处理的是控制流 reconvergence，而不是标量寄存器版本恢复

因此，对本项目当前输入范围而言，可以接受这样一个前提：

> 若某个值跨过 `join` 后仍以标量语义存活，它应当已经是 uniform 值，而不是需要在 `join` 点做 lane-wise merge 的值。

### 5.3 仍待实验进一步收敛的点

当前仍需实验继续收敛的关键点是：

- 在更大状态规模下，`value_blob` 相对 `vctx` 是否仍显著更优；
- 当 mutable state 规模接近真实 project 目标时，寄存器压力 / spill / stack 是否仍可接受；
- 具体 physical ABI 布局是否需要进一步分段或裁剪。

这些点在方向上已经有实验支持，但还不能写成“仓库内已被最终证明”的事实。

## 6. 当前实施顺序

当前建议的实施顺序是：

1. 先完成主提案与导航收束，让读者有稳定入口
2. 再推进 problem 1 / 3 / 4 的实现性设计与代码变更
3. problem 2 按独立路线继续推进
4. 在新主方案稳定后，将已被其完全取代的旧 PTX 设计稿归档

对真正的 lowering 实施，当前更稳妥的顺序是：

1. 先落 problem 4 + 3 的框架性收缩项
2. 再落 problem 2 的地址专门化
3. 最后完成 problem 1 的标量 canonical state 重构

但需要强调：
**problem 3 并不是完全独立的局部优化，它在语义上依赖 problem 1 / 4 的状态模型收敛。**

## 7. 验证标准

后续实现应至少围绕以下指标做真实 PTX / SASS 对比：

- PTX 指令总数
- `activemask/bfind/setp` 次数
- `ld/st.shared` 次数
- `bar.warp.sync` 次数
- `ld/st.local` 次数
- `ptxas -v` 的寄存器、stack、spill、lmem 指标

其中：

- problem 1 / 3 的直接目标是压 shared-x 与 block preamble 冗余
- problem 4 的直接目标是压 call 边界的 `.local` 样板成本
- problem 2 的直接目标是压普通访存模板膨胀

## 8. 与其它文档的关系

### 8.1 当前应优先阅读的文档

- `doc/PTX_LOWERING_MAIN_PROPOSAL.md`：本文，当前主入口
- `doc/PTX_LOWERING_REDUCTION_PLAN.md`：仍活跃的专项规划，尤其保留 problem 2
- `doc/IMPLEMENTATION_CODEMAP.md`：代码入口和调用链
- `doc/ventus-divergence-sgpr-analysis.md`：上游 LLVM 行为分析参考

### 8.2 历史设计/参考文档

以下文档仍保留，但其角色是**历史设计或背景参考**，不是当前主入口：

- `doc/PTX_LEADER_CTX_REUSE_DESIGN.md`
- `doc/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`
- `doc/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`

它们可以在以下场景按需阅读：

- 想看问题 3 的细化设计展开
- 想看 `value_blob` ABI 的详细状态分层
- 想追溯当时的方案争论与实验背景

## 9. 归档约束

当新的主提案和后续实现/导航真正稳定后：

- 被本文完全取代的 problem 1 / 3 / 4 相关旧 PTX 设计稿应移入 `doc/archive/`
- `doc/PTX_LOWERING_REDUCTION_PLAN.md` 保持在 `doc/`

保留 `doc/PTX_LOWERING_REDUCTION_PLAN.md` 的原因是：

- 它的 problem 2 内容仍然活跃；
- 本文并未试图取代该问题的专项规划。
