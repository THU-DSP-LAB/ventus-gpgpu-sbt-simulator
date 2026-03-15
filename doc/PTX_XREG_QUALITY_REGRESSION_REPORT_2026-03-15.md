# PTX xreg 质量回退评估（2026-03-15）

## 1. 背景

> 注：本文分析的是 replicated scalar-state 变更之前的 leader-lane 主线；当前实现已不再把它当作默认 lowering 语义。

`91bebb5` 引入了当前 leader-lane scalar state / value ABI / divergence shim 主线，用于修复上一阶段 SBT 方案中的 PTX 控制流回归。

该 change 在语义层面解决了若干 correctness 问题，但在落地后，需要重新评估两个问题：

- 生成的 PTX 文本质量是否仍可接受
- `ptxas` 编译后的 GPU 程序资源质量是否明显恶化

本报告对比：

- 新版本：`91bebb5` (`Fix leader-lane PTX control-flow regressions`)
- 旧版本：`3da36d6`（其父提交）

## 2. 评估方法

### 2.1 对比对象

使用同一环境分别构建两个版本的 `sbt_ptx`，对同一组 Rodinia kernel 生成 PTX，并用同一 `ptxas` 版本做 compile-first 验证。

评估样本为当前 `tools/rodinia_ptx_smoke.sh` 的 11 个 kernel：

- `findRangeK`
- `findK`
- `bpnn_layerforward_ocl`
- `bpnn_adjust_weights_ocl`
- `BFS_1`
- `BFS_2`
- `Fan1`
- `Fan2`
- `kmeans_kernel_c`
- `kmeans_swap`
- `NearestNeighbor`

### 2.2 工具与环境

- `ptxas`: CUDA 13.1 (`V13.1.115`)
- 目标架构：`sm_75`

### 2.3 指标

对每个 kernel 采集：

- PTX 行数、字节数
- `shfl.sync.idx.b32` 次数
- `ld.shared` / `st.shared` 次数
- `call.uni` 次数
- `ptxas -v` 的：
  - registers
  - stack frame
  - spill stores
  - spill loads
  - shared memory / constant memory
  - compile time

## 3. 结论摘要

结论很明确：当前 leader-lane 主线在 PTX / GPU 程序质量上出现了显著回退。

虽然 shared `x-state` 访存减少了，但替代成本更高：

- `xreg` 常驻 `%x<256>`
- 在 `vbranch/join` 边界执行 full-`x` broadcast
- 在 helper ABI 中按 blob 形式保存/恢复完整 `x/v` 状态

结果是：

- PTX 明显膨胀
- `ptxas` 编译时间显著上升
- 寄存器压力几乎全面顶到 `255`
- 大量 kernel 从 `0 spill` 退化为显著 spill

## 4. 量化结果

### 4.1 总体统计

相对旧版本，11 个 kernel 的平均变化如下：

- PTX 行数：`2.96x`
- PTX 字节数：`4.23x`
- `ptxas` 编译时间：`30.4ms -> 3020.7ms`
- 寄存器数：从 `18-34` 普遍升到 `247-255`
- 平均新增 stack frame：`252.4 bytes`
- 平均新增 spill stores：`1978.9 bytes`
- 平均新增 spill loads：`2540.4 bytes`
- `ld.shared` 降到原来的约 `32%`
- `st.shared` 降到原来的约 `34%`
- 新增 `shfl.sync.idx.b32`：平均每核 `2727.8` 次

### 4.2 代表性样本

#### `findRangeK`

- PTX 行数：`2556 -> 11557`
- 寄存器：`32 -> 255`
- stack frame：`0 -> 696`
- spill stores：`0 -> 10032`
- spill loads：`0 -> 14052`
- `ptxas` 时间：`67.8ms -> 18494.1ms`

#### `BFS_1`

- PTX 行数：`1172 -> 3993`
- 寄存器：`24 -> 255`
- stack frame：`0 -> 320`
- spill stores：`0 -> 1368`
- spill loads：`0 -> 1848`
- `ptxas` 时间：`28.5ms -> 1533.8ms`

#### `kmeans_kernel_c`

- PTX 行数：`1098 -> 4940`
- 寄存器：`25 -> 255`
- stack frame：`0 -> 448`
- spill stores：`0 -> 2812`
- spill loads：`0 -> 2148`
- `ptxas` 时间：`26.7ms -> 2717.8ms`

## 5. 根因分析

### 5.1 不是“声明了很多寄存器”，而是“这些寄存器被真实使用了”

旧版本同样声明了大量 PTX 寄存器名，但 `ptxas` 最终仍只分配 `18-34` 个寄存器。说明“寄存器名很多”本身不是主因。

当前回退的根因是：大量 `xreg` 被 emitter 提前 materialize 为真实 PTX 数据流状态。

具体表现：

- 声明 `.reg .b32 %x<256>;`
- leader-lane canonical scalar state 直接驻留在 `%x`
- 在 divergence / reconverge 边界执行 full-`x` broadcast
- 在 helper ABI 中保存/恢复完整 mutable state blob

这意味着 `%x1..%x255` 不再是“可能被后端删掉的冷状态槽位”，而是 CFG 上真实活跃的值。

### 5.2 control-flow 协议把几乎全部 `xreg` 拉进 live range

当前实现中：

- `emit_broadcast_full_x_state()` 会对 `%x1..%x255` 逐个发 `shfl.sync.idx`
- 该协议通过 path-entry / join-edge 标签插入到 `vbranch/join` 边界

因此，一旦某个 kernel 有结构化分歧，几乎整套 `xreg` 都会被显式使用。

这会直接带来：

- 更长的 live range
- 更高的寄存器压力
- `ptxas` 无法删除这些值
- 在高压下转为 stack / spill

### 5.3 这次回退的主因还不是 direct call

本次 11 个 Rodinia kernel 中，`call.uni` 计数全部为 `0`。

因此，当前看到的寄存器/stack/spill 爆炸，主要由：

- `leader-lane x-state`
- `full-x broadcast`
- `join/path-entry control protocol`

引起，而不是 helper direct call 先触发。

这说明当前最先需要处理的是 `xreg` 控制流协议，而不是优先优化 call ABI。

## 6. 与 lab/05 结论的关系

`lab/05_ptx_direct_call_param_abi` 中的实验结论并不与当前现象矛盾。

那组实验说明的是：

- 如果冷状态没有被 eager 地 materialize 成真实 use
- 并且后端只需保留语义上真正活跃的状态

那么 `ptxas` 确实可能把未真实使用的状态优化掉，或至少把成本显著推迟到更高压力区。

但当前主线并不满足这一前提：

- `xreg` 已常驻 `%x<256>`
- control-flow 边界对 full `x-state` 做显式 shuffle
- ABI 层面又把完整 `x/v` 做成语义可观察状态

换句话说，当前不是 `ptxas` 没有识别出 dead reg，而是 emitter 已经把大量本可保持冷态的 `xreg` 变成了真实、可观察、不可随意删除的 live state。

## 7. 代码证据

当前问题可以在 `sbt/ptx_emit.cpp` 中直接定位到以下实现点：

- `%x<256>` 声明
- `kNumXRegs = 256`
- `emit_broadcast_full_x_state()`
- `emit_store_mutable_state_blob()`
- `emit_restore_mutable_state_blob()`
- `prepare_control_protocol()`
- `emit_boundary_labels_for_block()`

这些路径共同决定了：

- `xreg` 在何时 materialize
- `xreg` 在哪些控制流边界被整套传播
- `xreg` 是否必须跨 helper 边界保真

## 8. 当前判断

当前 SBT 主线已经不能继续把 `xreg` 当作“整套常驻、整套广播、整套跨边界保真”的统一对象处理。

如果继续沿着这一路径推进：

- PTX 体积会继续膨胀
- `ptxas` 编译时间会继续变差
- 真实 GPU 程序资源占用会持续处于高压区
- 后续 direct call / nested divergence 只会把问题进一步放大

因此，当前必须开始着手解决 `xreg` 问题。

## 9. 后续工作的聚焦点

后续设计/实现需要围绕下面这个核心问题收敛：

> 哪些 `xreg` 在什么时刻必须 materialize 为真实 PTX state，哪些 `xreg` 不应在控制流边界被全量 broadcast，哪些状态不应默认跨边界保真。

需要特别避免继续维持以下模式：

- full-`x` broadcast 作为通用 control-flow 协议
- 完整 `x-state` 默认跨 helper / join / path-entry 保真
- 把“逻辑上可能存在的 xreg”直接等价成“物理上长期驻寄存器的 `%x`”

## 10. 最终结论

本次质量回退已经足够明确地表明：

- 当前 leader-lane 主线的 correctness 修补是有意义的
- 但其物理实现方式，特别是 `xreg` 的常驻与传播策略，不适合作为后续长期主线

当前最优先的工程任务已经不是继续堆叠局部修补，而是重新设计 `xreg` 的表示与控制流边界协议。
