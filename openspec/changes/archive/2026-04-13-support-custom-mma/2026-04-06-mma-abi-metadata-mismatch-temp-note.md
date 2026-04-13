# 2026-04-06 临时问题记录：MMA ABI / metadata mismatch

- 状态：active temporary note
- 发现日期：2026-04-06
- 性质：问题暴露 + 局部决策记录；用于约束当前实现策略，不直接改写 current spec 口径
- 发现来源：PTX lowering worker 在交叉核对 frontend builtin ABI、Spike shape window、active MMA 文档时发现三者存在未对齐点

## 背景

`support-custom-mma` 当前已经把以下内容写成 active contract 候选：

- `MmaInstInfo` 是 MMA lowering 的 first-class metadata；
- lowering 必须显式经过 `VGPR window -> logical tile -> PTX fragment tuple`；
- PTX operand ABI 需要通过显式 descriptor/tuple materialization 冻结，而不是在 emitter 中临时猜测。

但在继续推进 PTX tuple materialization 时，发现当前仓库内的 decode metadata、frontend 暴露的 builtin tuple ABI、以及 Spike 侧 shape 信息之间，至少在 `f16 -> f16` 的 `C/D` tuple 宽度解释上还没有统一口径。

这个问题目前只能认定为“发现了矛盾”，还不能认定“哪一侧一定是正确 contract”。在和你共同商议之前，不应把任何单一路径伪装成既成事实。

## 2026-04-07 决策更新（用户确认）

用户已确认当前策略为：

- 不把整个 MMA PTX lowering 写成“整体暂停/失败”；
- 仅把已确认受 ABI/metadata mismatch 影响的 `fp16 -> fp16` 路径在 sbtsim 中临时设为显式 fail-fast；
- 保留已编写的 MMA decode 与验证资产；
- 其余 MMA 路径继续作为 active change 范围推进，不被该问题整体阻断；
- 待 Ventus LLVM + Spike 工具链修复并澄清 contract 后，再回到 `fp16 -> fp16` 路径收敛。

## 关键证据

### 证据 1：frontend builtin ABI 对 `f16 -> f16` 给出的 tuple arity 小于 shape-only `C` window

在 `../llvm/clang/test/CodeGenOpenCL/ventus-mma-family.cl` 中，至少有以下两组直接证据：

- `m16n8k16 row.col f16->f16` 的 builtin 签名是 `uint4 a, uint2 b, uint2 c -> uint2`
- `m16n16k16 row.col f16->f16` 的 builtin 签名是 `uint4 a, uint4 b, uint4 c -> uint4`

如果把 builtin tuple arity 视为 frontend ABI 暴露给 kernel 作者、microtest、以及后续 tuple materialization 的直接 contract，那么这里的 `C/D` tuple 宽度分别是：

- `m16n8k16 f16->f16`: `c = 2`, `d = 2`
- `m16n16k16 f16->f16`: `c = 4`, `d = 4`

这与下面的 shape-only `cRegsPerThread` 数值并不一致。

### 证据 2：Spike 当前 `shape` 信息仍是 shape-only 口径

当前 Spike `../spike/riscv/ventus_custom.cc` 的 `shape` 信息对至少两个 family 给出的 `C` window 规模是：

- `m16n8k16`: `cRegsPerThread = 4`
- `m16n16k16`: `cRegsPerThread = 8`

也就是说，Spike 这里表达的是“按 shape 看，每线程 `C/D` window 需要多少个寄存器”，而不是“按当前 frontend builtin tuple ABI 暴露给 `f16->f16` 的参数/返回 tuple 需要多少个 `uint` 槽位”。

### 证据 3：当前本仓库 decode metadata 也是 shape-only 填充

当前仓库 decode 中：

- `sbt/riscv_decode.cpp` 的 `decode_mma_shape_info(...)` 直接把
  - `M16N8K16 -> c_regs_per_thread = 4`
  - `M16N16K16 -> c_regs_per_thread = 8`
  写死在 shape decode 表里；
- `sbt/riscv_decode.hpp` 的 `MmaInstInfo` 只有一个 `c_regs_per_thread` 字段；
- decode 填充 `cand.mma.c_regs_per_thread = shape.c_regs_per_thread`，未按 `cd_type` 分流。

因此，当前 `MmaInstInfo.c_regs_per_thread` 的实际含义更接近“shape-only 的物理/Spike window 大小”，而不是“frontend builtin / PTX tuple materialization 直接应消费的 tuple arity”。

## 当前矛盾的最小表述

以 `row.col f16->f16` 为例，至少存在下面这组未对齐：

| family | frontend builtin `c/d` tuple arity | Spike shape-only `cRegsPerThread` | decode `MmaInstInfo.c_regs_per_thread` |
| --- | --- | --- | --- |
| `m16n8k16` | `2` | `4` | `4` |
| `m16n16k16` | `4` | `8` | `8` |

因此当前还不能直接回答下面这些关键问题：

- `c_regs_per_thread` 的正式 contract 到底是“物理 VGPR window 大小”还是“frontend / PTX operand tuple arity”？
- `f16->f16` 是否天然意味着 `C/D` 以 packed-16 形式出现，因此 tuple 宽度应小于 shape-only window？
- 如果 `C/D` 需要同时存在“物理 window 宽度”和“tuple ABI 宽度”两个概念，哪一个应留在 `MmaInstInfo`，哪一个应进入独立 ABI descriptor？
- active 文档中出现的 `cRegsPerThread`，究竟应该被理解为 shape window metadata，还是已经隐含 frontend ABI 宽度？

这些问题现在都还是待确认项，不能跳步。

## 影响面

这个 mismatch 不是单点命名问题，而是会影响多个层次的 contract：

- decode metadata contract：
  `MmaInstInfo.c_regs_per_thread` 的语义若不澄清，后续 lowering 读取该字段时容易把 shape window 和 tuple ABI 混为一谈。
- PTX tuple materialization：
  如果 materializer 直接把 `c_regs_per_thread` 当作 tuple arity，`f16->f16` 的 `C/D` operand/result 组包很可能偏大。
- microtest / kernel ABI：
  kernel 侧 builtin 参数与返回值 tuple 如果已经按 frontend ABI 固化，测试输入输出载体会与 shape-only metadata 解释不一致。
- active 文档表述：
  现有 active 文档里关于 `cRegsPerThread`、VGPR window summary、tuple materialization 的描述，可能需要明确区分“shape window”与“ABI tuple”两个层面，避免读者默认它们是一回事。

## 目前不应做出的结论

在和你确认策略前，下面这些结论都不能直接写死：

- 不能直接下结论说 “Spike 错了”
- 不能直接下结论说 “frontend builtin ABI 错了”
- 不能直接下结论说 “`c_regs_per_thread` 必须按 `cd_type` 改写”
- 不能直接下结论说 “现有 active 文档已经无效”

目前能确定的只有：三处口径没有被同一套定义绑定起来，而 `fp16 -> fp16` 路径的 PTX lowering 已被这个缺口阻塞。

## 候选解决方向

下面列的是候选方向，不是建议你默认接受的结论。真正选型前需要与你共同商议。

### 方向 A：把 `MmaInstInfo.c_regs_per_thread` 改成 ABI-aware，按 `cd_type` 区分

思路：

- 保留 `shape` 作为 family 标识；
- 但把 `c_regs_per_thread` 定义成“lowering 直接消费的 `C/D` tuple arity”；
- 对 `f16->f16` 这类 packed-16 输出，令 `c_regs_per_thread` 小于 shape-only `C` window；
- shape-only 物理窗口若仍有需要，再引入单独字段表示。

潜在收益：

- lowering 和 tuple materialization 读取一个字段就能拿到 ABI 宽度；
- frontend builtin ABI 与 decode metadata 更容易对齐；
- `f16->f16` family 的 PTX operand/result tuple 生成路径更直接。

代价与风险：

- 会改变 `MmaInstInfo.c_regs_per_thread` 的语义，已有文档与代码阅读习惯都要同步重写；
- Spike shape-only 口径与 decode 字段名称可能明显脱钩，容易造成“同名不同义”；
- 如果其它 family 还需要物理 window 宽度，最后仍可能不得不新增第二组字段。

### 方向 B：保留 `c_regs_per_thread` 为 shape-only window，大方承认 ABI tuple 需要独立字段

思路：

- 继续把 `MmaInstInfo.c_regs_per_thread` 解释为“每线程物理 `C/D` window 大小”；
- 新增单独 ABI 元数据，例如 `c_tuple_regs_per_thread` / `d_tuple_regs_per_thread` 或放入 `PtxMmaAbiDesc`；
- lowering 中显式区分“Spike/VGPR window 层”和“frontend/PTX tuple 层”。

潜在收益：

- 最符合当前 decode 实现与 Spike shape-only 口径；
- 能把 `VGPR window -> logical tile -> PTX tuple` 的三层模型表达得更清楚；
- 避免把一个字段同时承担物理窗口与 ABI tuple 两种含义。

代价与风险：

- metadata 和 tuple materialization 接口会更复杂；
- 需要明确谁拥有 ABI tuple 宽度的最终真相：frontend builtin、native PTX descriptor，还是两者共同约束；
- active 文档里的 `cRegsPerThread` 表述可能必须系统性澄清，否则读者仍会误解。

### 方向 C：反向追认 shape-only 口径，调整 frontend builtin ABI / microtest 载体

思路：

- 认定 shape-only `cRegsPerThread` 才是唯一正式口径；
- 回头修改 frontend builtin 暴露方式或其测试期望，使 `f16->f16` 的 `C/D` tuple 宽度与 shape-only window 一致。

潜在收益：

- 解除了 shape-only 与 ABI-aware 双轨并存的复杂性；
- decode / Spike / lowering 使用同一组窗口宽度，数据流更单一。

代价与风险：

- 影响面最大，可能波及 frontend builtin 设计、已有 kernel 写法、microtest、以及外部使用者预期；
- 目前缺少证据证明 frontend ABI 必须服从这种改法；
- 如果 packed-16 tuple 原本就是有意设计，这条路会把真实 ABI 语义抹平，风险很高。

## 建议的决策入口

在真正决定前，至少需要你和我共同确认下面几件事：

1. `MmaInstInfo` 的首要职责是表达“物理窗口语义”，还是表达“lowering 直接可消费的 ABI 宽度”？
2. frontend builtin tuple ABI 是否已经被视作对 kernel / microtest 的外部 contract，还是仍允许主动修订？
3. `PtxMmaAbiDesc` 是否应该正式拥有 `C/D` tuple arity，而 `MmaInstInfo` 只保留 shape/window 语义？
4. active 文档中的 `cRegsPerThread` 是否需要拆成两个命名层次，避免继续一词多义？

在这几个问题与你共同商议并定策略之前，这份记录只用于暴露风险，不用于宣告哪条路线已经胜出。

## 当前结论

截至 2026-04-07，本问题的可靠结论有两条：

- 已发现 MMA frontend builtin ABI、Spike shape-only window、以及当前 decode metadata 在 `row.col f16->f16` 的 `C/D` 宽度解释上存在 mismatch。
- 当前已采用局部阻塞策略：仅 `fp16 -> fp16` 路径在 sbtsim 中显式 fail-fast，其他 MMA 路径不按“整体暂停”处理。

未决部分仍是 mismatch 的最终 contract 归一方案。后续应在 Ventus LLVM + Spike 工具链修复后回到该路径完成收敛。
