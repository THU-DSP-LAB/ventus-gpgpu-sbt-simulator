# PTX Lowering 主提案

> 状态：`historical archived proposal`
>
> 本文已从 `doc/` 迁入 `doc/archive/`。
>
> 迁移原因：
> - problems 1 / 3 / 4 已由当前主线解决或实质性收敛；
> - 当前剩余活跃问题只保留 problem 2，并已单独整理到 `doc/ADDRESS_SPACE_SPECIALIZATION.md`；
> - 本文保留为历史设计动机与方案收敛背景，不再作为 current 入口。
>
> 当前实现真相请优先以 `doc/IMPLEMENTATION_CODEMAP.md` 和 `openspec/specs/replicated-scalar-state/spec.md` 为准。

本文档曾是 PTX lowering 设计动机与边界说明的主入口，现仅保留为历史背景材料。
阅读顺序建议如下：

1. 本文
2. `doc/IMPLEMENTATION_CODEMAP.md`
3. `sbt/ptx_emit.cpp`
4. 如需背景，再按需阅读历史设计/分析参考

本文档的目标不是重新展开所有历史讨论细节，而是把 problems 1 / 3 / 4 已经落地的方案、边界和剩余优化轨道收敛为一份稳定、可长期引用的说明。后续 OpenSpec 变更可将本文作为 rationale reference，但不应再把它当作“尚未实现的 proposal”。

> 注：本文中若出现 shared `WarpCtx`、full-`x` broadcast、`.local vctx` 等表述，默认是在描述**方案落地前**的旧膨胀来源，而不是当前主线。

## 1. 当前角色与剩余目标

当前阶段的重点已经不是“定义 problems 1 / 3 / 4 的主方案”，而是：

- 固化已落地主线的 canonical 文档口径；
- 继续提升 **Ventus ELF -> PTX lowering 质量**；
- 把仍然活跃的问题与历史问题严格分层。

这里讨论的是 **生成 PTX 的质量**，不是 `sbt_ptx` 翻译器本身的运行速度。

## 2. 方案落地前的主问题划分（历史背景）

在主线落地前，曾识别出四类主要膨胀源。其中 problems 1 / 3 / 4 已经被当前实现主线实质性收敛；problem 2 仍然活跃。

### 2.1 问题 1：旧实现中标量 `x-reg` 驻留在 per-warp shared `WarpCtx`

旧实现里：

- 标量读写依赖 `ld.shared/st.shared`
- leader-only 标量更新后常带 `bar.warp.sync`
- 标量计算链无法自然留在 PTX 寄存器中

这是当时最大、最系统性的 PTX 膨胀源之一。

### 2.2 问题 2：普通访存大量走通用数值地址映射模板（当前仍活跃）

当前 `lw/sw/lb/lh/...` 和对应向量访存常展开为：

- shared/heap 判定
- pointer 计算
- ELF/heap 覆写
- 再做真实 load/store

这个问题仍然活跃，但当前已改由 `doc/ADDRESS_SPACE_SPECIALIZATION.md` 单独描述；原专项规划文档已归档。

### 2.3 问题 3：旧实现中 leader context 按 basic block 固定重建

旧实现中每个 block 入口都固定发：

```ptx
activemask.b32
bfind.u32
setp.eq.u32
```

即使 block 只包含纯向量算术，也要支付这三条固定税。

### 2.4 问题 4：旧实现中 direct call 通过整块 `vctx` spill / restore 维持向量状态

旧 direct call 方案会在 caller/callee 边界对 `%v0..%v255` 做整块 `.local` spill / restore。这对多函数 kernel 会引入大量 `ld.local/st.local` 与样板代码。

## 3. 当前已落地的总体方向

当前项目已经把 problems 1 / 3 / 4 收敛到下面这条主线：

- 用 replicated active-lane scalar state 取代 shared `WarpCtx.x[]` 作为默认 live scalar model；
- 把 leader 约束缩到真正需要 single-lane 语义的 use point，而不是对所有 scalar execution 维持永久 owner protocol；
- 把 direct call 切到 `mutable state + machine context + runtime env` 三层 value ABI；
- 把 `vbranch/join` 从 full-`x` payload / join shim 协议中解耦出来；
- 保留 problem 2 作为独立优化轨道继续推进。

可以概括成下面这张图：

```text
problem 1: x-reg live-state model
        \
         +-- replicated active-lane scalar state --+
        /                                          |
problem 3: leader derivation / use points          +-- layered value ABI
        \                                          |
         +-- lazy leader selection / no full-x ----+
        /
problem 4: call state transfer

problem 2: address specialization / PTX quality
  -> still active optimization track
```

## 4. 当前状态模型

### 4.1 `x-reg` 的当前 canonical live-state 口径

当前主线的口径是：

> live scalar state on the current active lanes is replicated and equal

也就是说：

- 函数体执行期间，默认不再以 shared `WarpCtx.x[]` 作为 live scalar state 的 authoritative 位置；
- 普通 scalar ALU / branch / load 等路径可以直接消费 replicated `%x`；
- single-lane 语义只在真正的 lane-sensitive / externally side-effecting use point 上显式处理。

### 4.2 leader 只在需要时按 use point 参与协议

当前主线不是“所有 scalar execution 都服从永久 leader-owned canonical model”，而是：

- all-lane scalar execution 不需要为了协议本身维护 path-entry full-state handoff；
- 当分歧路径真正遇到 leader-only 指令时，才从当前 active subset 中懒选择合法 leader；
- leader-only scalar write 之后，需要在后续 replicated-state consumer 观察到该状态之前完成 re-replication。

因此：

- `vbranch/join` 不再把整份 `x-reg` 作为默认控制流 payload；
- full-`x` broadcast / join shim 不再是默认正确性协议。

### 4.3 direct call 的当前方向是 layered value ABI

当前主线已经采用：

- 放弃以 `vctx` 为主线的 context ABI
- 采用聚合对象 value ABI
- 统一建模 mutable call state、只读 machine context、只读 runtime env

在这条线上，helper prototype / definition 必须同步反映当前 ABI 形状，且 call boundary 不再默认做 eager full-state materialization。

## 5. 当前接受的关键前提

本文必须明确区分两类东西：

- 本仓库当前代码已经体现的事实
- 本项目当前接受的上游/实验性信任边界

### 5.1 已知代码事实

当前代码事实包括：

- live scalar state 主线按 replicated active-lane scalar state 处理
- `vbranch/join` 主线不再插入 full-`x` payload / join shim
- direct call 已切到 `mutable/machine/runtime` 三层 value ABI

也就是说，本文描述的已不只是“接受的方向”，而是当前主线已经兑现的架构口径；更细的 as-built 行为仍应以代码与 `doc/IMPLEMENTATION_CODEMAP.md` 为准。

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

## 6. 后续实施顺序

当前建议的重点不再是实现 problems 1 / 3 / 4，而是：

1. 持续维护 current / target / historical 的文档分层
2. problem 2 按独立路线继续推进
3. 围绕当前主线继续做 PTX / SASS 质量压缩
4. 把已被当前主线取代的旧设计稿继续归档、降级或加状态标识

对真正的 lowering 优化，当前更稳妥的顺序是：

1. 继续推进 problem 2 的地址专门化
2. 基于当前 replicated scalar-state / value ABI 主线压缩框架冗余
3. 围绕 `ptxas -v`、spill、compile time 做真实回归比较

但需要强调：
**后续优化不应重新引入 shared `WarpCtx`、full-`x` broadcast 或 `.local vctx` 这类已被主线替代的协议。**

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

- `doc/archive/PTX_LOWERING_MAIN_PROPOSAL.md`：本文，历史主提案
- `doc/ADDRESS_SPACE_SPECIALIZATION.md`：当前仍活跃的问题 2 说明
- `doc/IMPLEMENTATION_CODEMAP.md`：代码入口和调用链
- `doc/ventus-divergence-sgpr-analysis.md`：上游 LLVM 行为分析参考

### 8.2 历史设计/参考文档

以下文档已经归档，但其角色仍是**历史设计或背景参考**，不是当前主入口：

- `doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`
- `doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`
- `doc/archive/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`

它们可以在以下场景按需阅读：

- 想看问题 3 的细化设计展开
- 想看 `value_blob` ABI 的详细状态分层
- 想追溯当时的方案争论与实验背景

## 9. 归档约束

当前已执行的归档约束：

- 被本文完全取代的 problem 1 / 3 / 4 相关旧 PTX 设计稿已移入 `doc/archive/`
- `doc/archive/PTX_LOWERING_REDUCTION_PLAN.md` 已归档

当前 active 入口已调整为：

- `doc/ADDRESS_SPACE_SPECIALIZATION.md`：problem 2（未来解决）
- `doc/IMPLEMENTATION_CODEMAP.md`：当前代码真相
