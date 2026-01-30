# PTX 硬件分歧/收敛 + CFG 结构化还原：可行性与方案（Ventus SIMT stack）

本文目的：在不软件化维护 `cur_mask/stack`（见 `simtstack_soft.md`）的前提下，评估能否通过 **CFG 结构化还原** 把 Ventus 的 `setrpc + vbranch + join` 翻译成“常规分支控制流”，并依赖 **PTX/NVIDIA 硬件的分歧/收敛机制** 来获得等价的 active mask 行为。

分析语义基准：`doc/ventus-isa/vbranch_simtstack.md`。

---

## 0. 结论（先给结论）

CFG 结构化还原 **在一组明确的输入约束下是可行的**：

- `vbranch` 的 join 点（`CSR_RPC`）必须能被静态解析为“函数内的某个确定 PC/label”。
- 对每个 `vbranch`，其 `rpc` 必须是该分支的 **后支配点（post-dominator）**，并且分支区域必须近似 **SESE（single-entry single-exit）**：两条路径最终都回到 `rpc`，且区域内部不应有“侧出口”。
- 程序应当满足常见的 SIMT 假设：warp 内无数据竞争/未同步的跨线程依赖，使得“先执行哪条路径”不可观测。

如果上述条件不能满足（尤其是 `rpc` 无法静态确定，或 CFG 不可归约/存在侧出口），则必须回退到 **软件 SIMT stack + 软件 mask** 的方案（`simtstack_soft.md`）。

---

## 1. 为什么这条路“可能可行”

Ventus 的分歧/收敛是显式的：

- `setrpc` 写 `CSR_RPC`，规定 join 的 PC。
- `vbranch` 基于 current mask 计算两条路径的新 mask，压栈两次，并先执行 PATH1。
- `join` 在 `pc == top.rpc` 时弹栈并恢复 `{pc, mask}`，否则 no-op；支持“共享同一 join 的嵌套分支”导致连续弹栈。

而在 NVIDIA/PTX 侧：

- 软件无法直接“写硬件 active mask”，但硬件分歧/收敛会在执行 `@p bra` 一类分支时自动维护 active mask 并在后支配点收敛。
- 若我们能把 Ventus 的显式 join 点转化为 PTX CFG 的 **后支配点**，那么硬件的 divergence stack 行为与 Ventus 的 SIMT stack 在宏观上是一致的：
  - 分歧时保存“另一条路径的 mask + join PC”。
  - 抵达 join 时恢复 mask 并切换到另一条路径/继续执行。
  - 共享 join 的嵌套分支在同一 join 点连续 pop。

因此，若能把 Ventus 程序还原成“结构化/可归约”的 CFG，并确保每个 `rpc` 就是该分支的收敛点，则可依赖硬件完成 mask 管理，从而不必对每条指令做 `@p_act` 谓词化。

---

## 2. 主要语义风险

### 2.1 PATH1（线程更少路径先执行）在 PTX 上很难精确复刻

Ventus 明确要求 PATH1=线程数更少的分支先执行（`doc/ventus-isa/vbranch_simtstack.md`）。

- 纯硬件分歧的 `@p bra` 并不提供“按 popcount 选择先执行哪条路径”的架构接口。
- 代码布局（把哪条路径放 fallthrough、哪条路径放 branch target）只能影响“哪条路径在 PC 上更近”，但无法保证硬件一定按 Ventus 的 PATH1 规则调度。

可行性结论依赖一个常见但需要显式写出来的假设：

- **输入程序在语义上不依赖同一 warp 内两条路径的执行先后次序**（例如不写出未同步的 warp 内数据竞争）。

若必须严格复刻 PATH1 顺序（例如你把 Ventus 的“顺序性”视为可观测 ISA 语义），则 CFG 结构化还原不足，需要软件化方案。

批注：PATH1 / PATH2 执行先后顺序并不重要，这是微架构实现的抉择，并不向编程者暴露为ISA语义，也不影响程序功能，因此此风险可忽略。

### 2.2 `CSR_RPC` 的可静态解析性是硬门槛

CFG 结构化还原要把 `rpc` 变成一个 PTX label（或至少是可静态定位的基本块入口）。

- `setrpc rd, rs1, offset` 的定义是 `rpc = rs1 + sext(offset)`。
- 如果 `rs1` 不是静态常量/可解析的“PC 值”，那么 `rpc` 可能是运行时值（哪怕 warp-uniform）。

而 PTX 侧无法“按运行时 PC 值跳到某个 label 并让硬件用它做收敛点”。

所以：

- **必须**对标量/warp-uniform 数据通路做常量传播与符号求值，把 `rpc` 解析为某个确定的 join 块（典型是编译器把 join 地址编码成 PC-relative 常量）。

### 2.3 join 的 no-op / 连续弹栈 与 后支配收敛的关系

Ventus `join` 的规则是“只有 `pc == top.rpc` 才 pop，否则 no-op”，并允许共享同一 join 造成连续 pop。

- 在结构化 CFG 中，`join` 通常被消解为一个纯 label（merge 点），并由硬件在该点完成“pop/恢复 mask”。
- 当没有对应的待收敛分支时，抵达 merge 点就等价于 `join` 的 no-op（直接顺序执行）。
- 共享 join 的嵌套分支：如果多个分支的后支配点就是同一个 merge label，硬件 divergence stack 也会在该 label 连续弹栈。

因此只要“`rpc` == 后支配点”成立，这一点是可对齐的。

---

## 3. 可行性的输入约束（建议在原型阶段直接写死）

为让硬件分歧/收敛可靠地等价 Ventus SIMT stack，建议把下列条件当作 **SBT 可接受输入**：

1) **`rpc` 静态可解**：每个 `vbranch` 使用的 `CSR_RPC` 必须能在编译期解析为函数内某个确定基本块 `J`。

2) **分支 SESE / 无侧出口**：
- 从 `vbranch` 的两条后继出发，所有路径最终都到达同一个 join 块 `J`；
- 该区域内部不允许跳到 `J` 之外的块（除非那条边也会在语义上“离开该分支区域”，并且在 PTX 侧能保持相同的后支配结构；原型阶段建议直接禁用）。

3) **`J` 是后支配点**：`J` 必须后支配该 `vbranch`（更严格地：应当是 immediate post-dominator，便于让 ptxas 生成常规的 reconvergence）。

4) **无间接控制流破坏结构**：`jalr`/computed jump、异常式的非结构化 goto 不出现在受 `vbranch/join` 约束的区域内。

5) **`barrier` 只在完全收敛处出现**：等价于 `cur_mask == FULL_MASK` 的点；在结构化 CFG 方案里，可近似要求 barrier 位于“不会处于分歧中的控制流点”。

6) **warp 内无可观测顺序依赖**：同一 warp 的 if/else 两条路径之间不通过未同步共享内存/原子序制造可观测次序差异。

---

## 4. 结构化还原方案（设计）

下面给出一套“能落地实现”的结构化还原管线，但此文件只描述设计，不写代码。

### 4.1 IR 与基本分析

- 反汇编/解码得到线性指令流，按 PC 切基本块，构建 CFG：
  - 显式控制流边：`jal/jalr/branch/vbranch`。
  - `join` 本身是 0/1 次 pop+跳转的显式控制流，但在结构化方案里我们倾向把它变成 merge label（见 4.4）。

- 标量/warp-uniform 值分析（最少要做）：
  - 目标：在每条 `vbranch` 处求出“当前 `CSR_RPC` 对应的 join PC”。
  - 方法：对标量寄存器/CSR 做常量传播 + 简单符号求值（支持 `auipc`/加法/立即数）。
  - 若求不出确定 join：标记该 `vbranch` 为“不可结构化”，回退软件方案。

### 4.2 从 Ventus 显式语义提取“分支区域”

对每条 `vbranch`，定义：

- `B`：`vbranch` 所在基本块
- `S0`：fallthrough（`pc+4`）
- `S1`：taken（`pc+offset`）
- `J`：join 块（`rpc` 解析得到的 PC 对应的基本块入口）

构造候选区域 `R(B,J)`：从 `S0/S1` 出发，在 CFG 上向前遍历，直到抵达 `J` 为止（不穿过 `J`）。

### 4.3 区域合法性校验（结构化是否成立）

对候选区域做检查（原型阶段宁可严格一点）：

- **可达性**：`S0 ->* J` 且 `S1 ->* J`。
- **后支配**：`J` 后支配 `B`。
- **无侧出口**：区域内任意块 `X` 的后继若不在区域内，则必须是 `J`（或被允许的“结构化出口”，原型阶段建议不允许）。
- **单入口**：区域内除 `B` 外的块不应有来自区域外的前驱（否则 region 不是 SESE）。

若任意失败：不做 CFG 结构化（回退软件 SIMT stack）。

### 4.4 用 region 树还原嵌套关系（含共享 join）

Ventus 的 SIMT stack 天然描述了嵌套结构：

- 一条 `vbranch` 把 join 点写在 `rpc`，并把 join 行为延后到 `join` 处。
- 共享 join 的嵌套分支对应 region 树中“多个 region 的 `J` 相同”。

可行的一种构造方法：

1) 按 CFG 的支配关系构建 region 树：
- 外层 region 的 `B` 支配内层 region 的 `B`；
- 内层 region 的节点集合是外层 region 的子集；
- 允许多个 region 共享同一 `J`。

2) 或者按线性 PC 扫描（更贴近 Ventus 栈语义）建立一个“静态栈”：
- 扫描指令，遇到 `vbranch` 时把 `J` 入栈；
- 遇到位于某个 `J` 的 `join`（或块入口）时弹出所有 top==J 的条目。

注意：第二种方法依赖“代码布局与可达性”较规整；若遇到循环/跳转，仍需用 CFG 校验兜底。

### 4.5 生成 PTX 控制流（依赖硬件收敛）

核心策略：把每个 Ventus `vbranch` 翻译为一次“普通的 per-thread 条件分支”，让硬件自己维护 active mask：

- `setrpc`：
  - 若程序可能读取 `CSR_RPC`：保留一个翻译后的变量/寄存器表示它（写入解析得到的 `J` 的“逻辑 PC/块 id”）。
  - 若确认仅用于 join：可在控制流层面忽略（join 信息已被结构化还原吸收）。

- `vbranch`：
  - 生成 per-thread predicate `p_cond`（由向量寄存器比较得到）。
  - 发出 `@p_cond bra L_then` 形式的分支（或相反布局）。
  - 以常规方式组织 then/else 两段代码，并在末尾用一条无条件 `bra` 跳到 merge label（避免 fallthrough 执行两边）。
  - merge label 选择 `L_join`，对应 Ventus 的 `rpc`/`J`。

- `join`：
  - 在结构化方案里不再生成“指令级 join”，而是把它当作 `L_join:` 标签位置。
  - Ventus 的“join 可能 no-op”的情况自然成立：如果没有发生分歧，控制流到达 `L_join` 就是顺序继续。

这样 ptxas 有机会从 CFG 推导出 `L_join` 是后支配点，并在底层 SASS 上生成恰当的 reconvergence（例如 ssy/sync stack 行为）。

---

## 5. 何时必须回退到软件 SIMT stack

只要出现下面任一情况，结构化还原就不应硬上：

- `rpc` 无法静态解析为确定 join。
- `rpc` 解析得到的 `J` 不是 `vbranch` 的后支配点，或区域存在侧出口/多入口。
- CFG 不可归约（irreducible），或存在间接跳转破坏结构。
- 你需要严格匹配 PATH1（较小线程数路径先执行）的可观测顺序。

这些都是软件方案（`simtstack_soft.md`）更合适的适用场景：软件方案用 `ballot/popcount` 显式选择 PATH1、显式 push/pop，语义最贴近 Ventus 文档。

---

## 6. 原型阶段的建议“验证闭环”

如果要验证结构化方案是否工作，建议按最小化场景逐步放宽约束：

1) 只允许 `setrpc` 直接写入“常量 join label”（最容易静态解析）。
2) 只允许 SESE if/else，无侧出口、无循环。
3) 引入共享 join 的嵌套 if（验证连续 pop/共享收敛点）。
4) 最后引入循环（要求 loop header/exit 可结构化，且 join 点仍是后支配）。

一旦任何一步失败，优先用“不可结构化 -> 回退软件 simtstack”的方式保证正确性，而不是尝试用未验证的 CFG hack。
