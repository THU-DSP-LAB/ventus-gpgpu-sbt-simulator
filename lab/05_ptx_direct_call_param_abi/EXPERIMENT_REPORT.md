# PTX Direct Call 参数 ABI 实验报告

## 0. 术语与情节说明

本实验报告里的几个关键词，含义如下。

### 0.1 三种 ABI 方案

- `vctx`
  - 这是对当前主线实现思路的抽象模拟。
  - caller 不把状态拆成显式参数，而是先把整份状态写进一块 `.local` 上下文区 `vctx`。
  - callee 通过 `vctx` 指针把整份状态再读出来，返回前再写回，caller 返回后再恢复。
  - 可以把它理解成“context ABI”。

- `value_params`
  - 这是“分散 value ABI”的代理模型。
  - 状态不再经过共享上下文块，而是被拆成大量离散标量参数，逐个通过调用接口传递。
  - 在本实验里，输入是多个 `.param .b32`；返回值由于 PTX 不接受超大多 `.param` 返回，因此用多个 `.reg` return 近似。
  - 可以把它理解成“把状态显式展开成一长串独立参数”。
  - 注意：这组 case 会触发 `ptxas` 的 `disabling ABI` warning，因此它不是“标准 PTX direct-call 参数 ABI”的严格对照组，只能作为一个探索性 proxy。

- `value_blob`
  - 这也是 value ABI，但不是把状态拆成很多离散参数，而是聚合成一个大对象。
  - 在本实验里，这个大对象表现为单个 `.param .b8 blob[...]` 输入/返回块。
  - 可以把它理解成“状态仍然走 value ABI，但参数表面形状是一个聚合对象”。

### 0.2 三种状态使用情节

- `hot4`
  - 表示 callee 实际只真正使用并修改前 4 个状态字。
  - 但 caller 仍然会观察全部状态，因此其余状态不是 dead value，而是必须被正确 passthrough 的冷状态。
  - 这个情节对应 brainstorm 里“如果大接口里只有少量状态真正有用，后端能不能自己压缩冷状态”这个问题。

- `hot4_dead`
  - 表示 callee 只真正使用并修改前 4 个状态字，caller 也只观察前 4 个状态。
  - 因而其余状态在 caller 侧是 dead value，可用于对照“deadcode 消除会把 ABI 成本删到什么程度”。

- `full`
  - 表示 callee 会使用并修改全部状态字，caller 也会对全部状态参与后续计算。
  - 这个情节对应最坏情况，即“full-state value ABI”没有冷状态可裁剪时，成本是否仍优于 `vctx`。

### 0.3 本报告里的“情节”是什么意思

这里的“情节”不是业务语义，而是微基准里人为构造的状态使用模式，用来模拟不同 direct-call 压力场景：

- `hot4` = “大接口，但真实热点很少”
- `hot4_dead` = “大接口，但冷状态在 caller 侧完全不可观察”
- `full` = “大接口，而且所有状态都活跃”

## 1. 结论摘要

基于本目录微基准的首轮实测，当前可以先给出 5 个结论：

1. 在本实验结构下，三种 ABI 的 case 在 SASS 中都保留了 1 个 `CALL`。
   - 默认 `./run_experiment.py` 运行矩阵包含 `54` 个 case（`hot4`/`hot4_dead`/`full`）
   - 其中主分析矩阵的 `36` 个 case（`hot4`/`full`）全部 `call_count=1`
   - 补充抽查 `-O0` 的 9 个 case 仍然全部 `call_count=1`
   - 但 `call-window LDL/STL` 普遍不大，这只说明 call 边界附近的 local-memory 型搬运不大，不能直接等同于整体 marshalling 成本已经很低

2. 在修正后的 `hot4` 场景下，冷状态一旦在 caller 侧保持可观察，value ABI 并不会“免费压掉”全部冷状态。
   - `value_params` / `value_blob` 的寄存器压力会随 `state_words` 增长，在 `s256/hot4` 已顶到 `255` 个寄存器
   - 但它们仍然明显少于 `vctx` 的 local 搬运和 stack 成本

3. 在 `full` 场景下，三种方案的代价形状明显不同：
   - `vctx`：PTX 文本较短，但 local traffic 和 stack 明显最大
   - `value_params`：显式 `spill` 为 0，但在 `256/full` 场景已经出现 `lmem`
   - `value_blob`：PTX 文本接近 `vctx`，但在 `256/full` 场景开始出现 stack/spill

4. 若只看 PTX 文本规模，`value_blob` 明显优于分散参数 `value_params`。
   - 例如 `s256/full/o32`：
     - `vctx`: `337028`
     - `value_blob`: `340943`
     - `value_params`: `372331`

5. 这轮实验仍支持继续探索 value ABI，但更值得优先追的是**聚合 blob / 聚合对象**；`value_params` 由于触发 ABI disable，不应作为正式主结论的核心依据。

补充：

- `hot4_dead` 作为补充对照仍有价值，因为某些真实 Ventus 程序确实可能完全不使用某些跨调用状态槽。
- 但它回答的是“冷状态在 caller 侧不可观察时，编译器能删掉多少成本”，不能替代 `hot4` passthrough 情节。
- `value_params` 的 `ABI disable` 不会强制 inline；当前 case 的 SASS 里仍然保留 `CALL`。
- 但它会让这组结果混入 `ptxas` fallback marshalling 成本，因此不再是“标准 scalar direct-call ABI”的干净测量。
- 目录内现已补 `runtime_validate.cc`，可以对指定 case 做 CUDA runtime 功能一致性抽查。

## 2. 实验目的

本实验对应 [`doc/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`](../../doc/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md) 中提出的几个核心问题：

- `vctx` 和 `.param value ABI` 相比，PTX 是否更短
- `ptxas` 能否自动压掉“大接口里的无用状态”
- 聚合参数和分散参数哪种更值得继续
- `ptxas` 会不会把 call 边界自动优化掉
- 大接口是否会把问题转化成 stack/spill/local traffic

本轮故意不改主线 `sbt/ptx_emit.cpp`，而是用手写/生成 PTX 微基准隔离 ABI 形状本身的影响。

## 3. 实验环境

- 仓库路径：`/work/ventus-env/sbtsim_lab`
- CUDA 工具链：
  - `ptxas`: CUDA 13.1, `V13.1.115`
  - `cuobjdump`: CUDA 13.1, `V13.1.115`
- 默认目标架构：`sm_75`
- 默认优化级别：`ptxas -O3`

工具版本命令：

```bash
ptxas --version
cuobjdump --version
```

## 4. 试验设计

### 4.1 ABI 变体

- `vctx`
  - caller 把全部状态写入 `.local vctx`
  - callee 读回全部状态
  - 返回前写回，caller 返回后再恢复
- `value_params`
  - 输入状态拆成离散 `.param .b32`
  - 返回状态由于 PTX 不接受“多个 `.param` 返回值”，改为多个 `.reg` return
  - 这是“分散 value ABI”的乐观 proxy，不是最终 ABI 语法承诺
- `value_blob`
  - 输入/返回都走单个聚合 `.param .b8 blob[...]`

### 4.2 默认运行矩阵与主分析矩阵

默认 `-O3` 运行矩阵：

- 脚本默认运行矩阵：
  - `state_words`: `32`, `128`, `256`
  - `use_mode`: `hot4`, `hot4_dead`, `full`
  - `helper_ops`: `4`, `32`
  - `abi`: `vctx`, `value_params`, `value_blob`
  - 共 `3 x 3 x 2 x 3 = 54` 个 case

- 本报告主分析矩阵：
  - `state_words`: `32`, `128`, `256`
  - `use_mode`: `hot4`, `full`
  - `helper_ops`: `4`, `32`
  - `abi`: `vctx`, `value_params`, `value_blob`
  - 共 `3 x 2 x 2 x 3 = 36` 个 case

补充 `-O0` 抽查矩阵：

- `state_words=32`, `helper_ops=4`, `use_mode=hot4/full`, 全部 3 种 ABI
- `state_words=256`, `helper_ops=32`, `use_mode=full`, 全部 3 种 ABI

共 9 个 case。

补充 deadcode 对照矩阵：

- `state_words`: `32`, `128`, `256`
- `use_mode`: `hot4_dead`
- `helper_ops`: `4`, `32`
- `abi`: `vctx`, `value_params`, `value_blob`

共 18 个 case。

### 4.3 指标

- PTX 规模：`ptx_bytes`, `ptx_lines`
- PTX 指令形状：`.param` / `ld.param` / `st.param` / `ld.local` / `st.local`
- `ptxas -v`：`registers`, `lmem_bytes`, `stack_frame`, `spill_stores_total_bytes`, `spill_loads_total_bytes`
- SASS：`CALL` 数、`LDL/STL` 数，以及 call 窗口附近的 `LDL/STL`

补充说明：

- `lmem_bytes`
  - 表示每线程 local memory 总占用。
  - 它不只包含寄存器 spill，也可能包含 call ABI 临时搬运区或编译器生成的 local scratch。
  - 因此 `spill=0` 不等于“完全没有 local memory 成本”。
- `spill_stores_total_bytes` / `spill_loads_total_bytes`
  - 表示 caller + callee 全部函数属性块的 spill 字节总和。
  - 它和 `LDL/STL` 不是同一量纲：前者是字节数，后者是整份 SASS 的 local 指令条数。
- `value_params`
  - 这组 case 触发 `ptxas warning: Function 'helper_value_params' with multiple return values found, disabling ABI`
  - 因而它测到的是 `ptxas` 对该非法/超规函数形状的 fallback lowering，不应直接当成“标准 scalar value ABI”的结果
  - 不利影响是：寄存器、`lmem`、`LDL/STL`、call 周边搬运模式，都会混入 fallback marshalling 成本，而不是纯 ABI 本身的成本

- 为了更直接比较 ABI 方案，建议优先看三类“综合成本”：
  - `LDL + STL`：SASS 层总 local 访存条数
  - `call-window LDL/STL`：`CALL` 附近窗口内的 local 访存，更接近 direct-call ABI 边界附近的 local-memory 型成本
  - `lmem_bytes`：每线程 local 空间占用

## 5. 实验命令

默认矩阵：

```bash
cd /work/ventus-env/sbtsim_lab/lab/05_ptx_direct_call_param_abi
./run_experiment.py
```

补充 `-O0` 抽查：

```bash
./run_experiment.py --ptxas-opt-level=-O0 \
  --state-words 32 --helper-ops 4 --modes hot4 full \
  --abis vctx value_params value_blob \
  --out-dir build_O0_s32

./run_experiment.py --ptxas-opt-level=-O0 \
  --state-words 256 --helper-ops 32 --modes full \
  --abis vctx value_params value_blob \
  --out-dir build_O0_s256_full_o32

./run_experiment.py --modes hot4_dead --out-dir build_hot4_dead
```

## 5.1 微基准 kernel 流程

这套实验不是直接拿真实 Ventus kernel，而是构造了一个固定形状的 PTX 微基准：

1. caller 初始化 `state_words` 个 32-bit 状态槽
2. 通过某种 ABI 形式把状态传给 callee（vctx/value_params/value_blob三种ABI形式）
3. callee 读入状态并按 `hot4` 或 `full` 情节修改
4. caller 取回结果，对状态做 checksum（三种ABI形式）
5. caller 把 checksum 写到 `out_ptr[tid]`

三种 ABI 的主要差异在步骤 2~4：

- `vctx`：caller 写 `.local vctx`，callee 从 `vctx` 全量读回/写回，caller 再读回
- `value_params`：caller 写离散 `.param`，callee 读入后通过多返回寄存器返回
- `value_blob`：caller 写聚合 `blob` 参数，callee 读入并写回返回 `blob`

注：

- 当前版本已修正 `hot4` 语义：caller 会对全部状态做校验，避免把冷状态错误地做成 dead value。
- 因此本报告中的 `hot4` 数据是“冷状态需保持 passthrough 语义”的结果，不是“冷状态可直接删掉”的结果。
- `hot4_dead` 则保留了旧版 deadcode 情节，用于补充说明“不可观察冷状态”对优化结果的影响。

## 6. 默认 `-O3` 实测结果

### 6.1 `hot4`：callee 只使用前 4 个状态

| case | regs | lmem | stack | spill_total(st/ld bytes) | calls | LDL/STL | call-window LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx_s32_hot4_o4` | 40 | 0 | 128 | 0/0 | 1 | 64 | 9 | 8335 |
| `value_params_s32_hot4_o4` | 38 | 0 | 0 | 0/0 | 1 | 0 | 0 | 12467 |
| `value_blob_s32_hot4_o4` | 40 | 0 | 0 | 0/0 | 1 | 0 | 0 | 8885 |
| `vctx_s128_hot4_o4` | 62 | 0 | 512 | 0/0 | 1 | 256 | 10 | 30359 |
| `value_params_s128_hot4_o4` | 136 | 0 | 0 | 0/0 | 1 | 0 | 0 | 47511 |
| `value_blob_s128_hot4_o4` | 135 | 0 | 0 | 0/0 | 1 | 0 | 0 | 32349 |
| `vctx_s256_hot4_o4` | 62 | 0 | 1024 | 0/0 | 1 | 512 | 10 | 60336 |
| `value_params_s256_hot4_o4` | 255 | 32 | 0 | 0/0 | 1 | 16 | 6 | 95639 |
| `value_blob_s256_hot4_o4` | 255 | 0 | 0 | 0/0 | 1 | 0 | 0 | 64251 |

观察：

- 修正后可见：只要 caller 继续观察全部状态，`value_params` / `value_blob` 的寄存器压力也会随 `state_words` 明显上升
- 这说明此前“冷状态被自动压缩到几乎免费”的结论不成立，之前主要混入了 dead-value elimination
- 即便如此，`value_blob` 在 `hot4` 下仍几乎没有 local traffic，而 `vctx` 的 stack 和 `LDL/STL` 则几乎正比于总状态规模
- `value_params` 在 `s256/hot4` 还出现了 `lmem=32` 与少量 `LDL/STL`；但这组结果需结合 ABI disable 警告谨慎解读

### 6.1.1 `hot4_dead` 补充对照：caller 不观察冷状态

补充矩阵目录：`build_hot4_dead/`

代表性对照（`helper_ops=4`）：

| case | regs | lmem | stack | LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vctx_s32_hot4_dead_o4` | 40 | 0 | 128 | 50 | 7551 |
| `value_params_s32_hot4_dead_o4` | 10 | 0 | 0 | 0 | 11683 |
| `value_blob_s32_hot4_dead_o4` | 12 | 0 | 0 | 0 | 8101 |
| `vctx_s128_hot4_dead_o4` | 62 | 0 | 512 | 194 | 26843 |
| `value_params_s128_hot4_dead_o4` | 10 | 0 | 0 | 0 | 43995 |
| `value_blob_s128_hot4_dead_o4` | 12 | 0 | 0 | 0 | 28833 |
| `vctx_s256_hot4_dead_o4` | 62 | 0 | 1024 | 386 | 53108 |
| `value_params_s256_hot4_dead_o4` | 10 | 0 | 0 | 0 | 88411 |
| `value_blob_s256_hot4_dead_o4` | 12 | 0 | 0 | 0 | 57023 |

与 `hot4` passthrough 版对比，差异非常直接：

- `value_params` / `value_blob` 会从“随状态规模上升的高寄存器压力”退回到几乎固定的低寄存器数
- `vctx` 仍然保留整块 `.local` 搬运，因此 deadcode 情节对它的改善幅度远小于 value ABI
- 这说明 `hot4_dead` 的主要回答是：“如果冷状态在 caller 侧完全不可观察，编译器能删掉多少 ABI 成本”
- 因此它可以作为补充情节保留，但不能替代 `hot4` passthrough 版来回答“冷状态仍需正确保留时”的问题

### 6.2 `full`：callee 修改全部状态

| case | regs | lmem | stack | spill_total(st/ld bytes) | calls | LDL/STL | call-window LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx_s32_full_o32` | 53 | 0 | 128 | 0/0 | 1 | 64 | 10 | 41109 |
| `value_params_s32_full_o32` | 64 | 0 | 0 | 0/0 | 1 | 0 | 0 | 45241 |
| `value_blob_s32_full_o32` | 64 | 0 | 0 | 0/0 | 1 | 0 | 0 | 41659 |
| `vctx_s128_full_o32` | 132 | 0 | 512 | 0/0 | 1 | 256 | 10 | 165765 |
| `value_params_s128_full_o32` | 167 | 0 | 0 | 0/0 | 1 | 0 | 0 | 182917 |
| `value_blob_s128_full_o32` | 168 | 0 | 0 | 0/0 | 1 | 0 | 0 | 167755 |
| `vctx_s256_full_o32` | 255 | 0 | 1656 | 1292/1292 | 1 | 1157 | 10 | 337028 |
| `value_params_s256_full_o32` | 255 | 388 | 0 | 0/0 | 1 | 422 | 6 | 372331 |
| `value_blob_s256_full_o32` | 255 | 0 | 504 | 1084/1084 | 1 | 542 | 6 | 340943 |

观察：

- `s32/full` 与 `s128/full` 下，`value_blob` 几乎同时拿到：
  - 接近 `vctx` 的 PTX 文本体积
  - 零 stack / 零 spill / 零 local traffic
- 到 `s256/full/o32` 时，三者开始分化：
  - `vctx`：stack 最大，local traffic 最重，且 helper spill total 已上升到 `1292/1292 bytes`
- `value_params`：`stack/spill=0`，但 `lmem=388` 且 `LDL/STL=422`；同时它触发 ABI disable，因此只可作辅助观察
  - `value_blob`：PTX 仍接近 `vctx`，但开始出现明显 stack/spill；其 total spill 已到 `1084/1084 bytes`

### 6.3 `helper_ops` 影响

对比 `o4` 与 `o32`：

- `hot4` 场景下，三种 ABI 的寄存器数变化都很小
- `full` 场景下，`helper_ops` 主要拉高 PTX 文本大小；对 `vctx` 的 local 搬运条数几乎没有帮助
- 最极端的是 `vctx_s256_full_o32`：helper 本体增大后，仍叠加 `512 + 512` 条 PTX 级 `ld/st.local`

## 7. `-O0` 补充结果

### 7.1 结论

即使降到 `ptxas -O0`，抽查 case 仍然全部 `call_count=1`。这意味着：

- helper call 边界在 `-O0` 下并没有消失
- 但 call 周边的局部 `LDL/STL` 仍然不大，这只能说明边界附近的 local-memory 型搬运仍不重

### 7.2 代表性数据

| case | opt | regs | lmem | stack | spill_total(st/ld bytes) | LDL/STL | calls |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx_s32_hot4_o4` | `-O0` | 43 | 0 | 128 | 0/0 | 100 | 1 |
| `value_params_s32_hot4_o4` | `-O0` | 38 | 0 | 0 | 0/0 | 0 | 1 |
| `value_blob_s32_hot4_o4` | `-O0` | 39 | 0 | 0 | 0/0 | 0 | 1 |
| `vctx_s256_full_o32` | `-O0` | 255 | 0 | 1128 | 164/164 | 1106 | 1 |
| `value_params_s256_full_o32` | `-O0` | 255 | 64 | 0 | 0/0 | 86 | 1 |
| `value_blob_s256_full_o32` | `-O0` | 255 | 0 | 64 | 172/172 | 86 | 1 |

观察：

- `-O0` 下，`vctx` 的局部访存仍然极高，但 total spill 反而小于 `-O3`
- `value_params` 仍然是 stack/spill 最干净的方案
- `value_blob` 在 `-O3` 下 helper spill 反而更重；这说明 `-O3` 可能把更多压力推到 helper 的激进调度/寄存器分配上

### 7.3 运行时功能一致性抽查

为回答“这些 PTX 能不能单独跑起来，并比较 ABI 语义是否一致”，本目录现已补了一个 CUDA host wrapper：

- 源码：`runtime_validate.cc`
- 构建脚本：`build_runtime_validate.sh`

它通过 CUDA Driver API 直接加载本实验生成的 `PTX`，在当前 GPU 上 JIT，然后：

- 运行 `kernel_vctx` / `kernel_value_params` / `kernel_value_blob`
- 将每个 ABI 的输出与 CPU 参考实现逐 lane 比较
- 再做 ABI 之间逐 lane 交叉比对

当前已抽查通过的代表性 case：

- `s32/hot4/o4`
- `s256/full/o32`

这说明：

- 这些微基准 PTX 不是“只能静态分析、无法执行”的文本产物
- 至少在上述代表性 case 上，三种 ABI 的运行结果与 CPU 参考实现一致

但要注意：

- 当前微基准 kernel 只使用 `%tid.x`，没有使用 `%ctaid.x`，因此 wrapper 采用“单 block、多 lane”方式验证
- 当前 wrapper 还没有并入 `run_experiment.py` 去自动跑完整矩阵
- 因而本报告表格本身仍然主要是“静态 PTX/SASS 成本报告”，runtime 抽查只作为补充验证

## 8. 结果解释

### 8.1 这轮实验已经回答到的问题

已经能回答：

- `vctx` 的固定 `.local` 搬运代价确实非常硬，且随状态规模线性增长
- 在 `hot4_dead` 情节下，后端对 value ABI 的 deadcode 压缩能力明显强于 `vctx`
- 在 `hot4` passthrough 情节下，这种“几乎免费”的压缩结论不再成立，value ABI 仍然要承担显著寄存器压力
- 在“full-state 全量活跃”时，聚合 blob 比分散参数更有希望成为主线方向，因为它的 PTX 文本体积更可控

### 8.2 这轮实验没有回答到的问题

还没有回答：

- 完整默认矩阵上的三种 ABI 结果是否都已经做过系统化 runtime 验证
- 如果真实 helper 边界无法被内联，`.param` value ABI 还是否同样占优
- `CALL` 真正保留时，除 local-memory 指令之外是否仍有其它显著参数搬运
- occupancy 是否会在真实 kernel 上恶化
- 若把 `value_params` 改造成一个合法、未触发 ABI disable 的 scalar ABI 变体，结果会如何变化

原因很明确：

- 当前主表格只做了静态 PTX/SASS 分析；虽然现在已经补了 runtime wrapper，但它还没有覆盖完整矩阵
- `call-window LDL/STL` 只覆盖 local-memory 型边界搬运，不能直接代表全部参数 marshalling
- 因而测到的是“当前微基准形状下，保留单个 call 边界时，各 ABI 方案的静态编译成本轮廓”

## 9. 对主线实现的含义

### 9.1 可以先采纳的方向性判断

- 可以继续探索 value ABI
- 聚合对象 / blob 比超大分散参数更值得优先做
- 不建议先投入 helper 摘要裁剪来优化现有 `vctx`
- `hot4_dead` 说明：若真实 Ventus 程序里某些状态在 caller/callee 两侧都不可观察，value ABI 确实更有机会被编译器大幅裁剪

### 9.2 还不应直接下结论的点

- 不能仅凭本实验就宣布“value ABI 一定全面替代 `vctx`”
- 因为这里观察到的优势主要建立在当前微基准的静态编译结果上，且 `call-window LDL/STL` 只覆盖 local-memory 型边界搬运
- 目前也还没有把 runtime 功能一致性验证扩展到完整矩阵
- 也不能把当前 `value_params` 结果当成“标准 scalar value ABI”的结论，因为它已经触发 ABI disable
- 因而当前更稳妥的主对照应是 `vctx` vs `value_blob`，`value_params` 只保留作探索性旁证

## 10. 复现方案

### 10.1 环境检查

```bash
cd /work/ventus-env/sbtsim_lab
ptxas --version
cuobjdump --version
g++ --version
python3 -m py_compile lab/05_ptx_direct_call_param_abi/run_experiment.py
```

### 10.2 运行默认矩阵

```bash
cd /work/ventus-env/sbtsim_lab/lab/05_ptx_direct_call_param_abi
./run_experiment.py
```

这条命令会生成默认 `54` 个 case；其中报告主分析通常聚焦 `hot4/full` 的 `36` 个 case，`hot4_dead` 作为补充对照单独解读。

产物：

- `build/generated_ptx/*.ptx`
- `build/cubin/*.cubin`
- `build/sass/*.sass`
- `build/summary.csv`
- `build/REPORT.md`

### 10.3 构建 runtime validator

```bash
cd /work/ventus-env/sbtsim_lab/lab/05_ptx_direct_call_param_abi
./build_runtime_validate.sh
```

### 10.4 运行补充 `-O0` 矩阵

```bash
./run_experiment.py --ptxas-opt-level=-O0 \
  --state-words 32 --helper-ops 4 --modes hot4 full \
  --abis vctx value_params value_blob \
  --out-dir build_O0_s32

./run_experiment.py --ptxas-opt-level=-O0 \
  --state-words 256 --helper-ops 32 --modes full \
  --abis vctx value_params value_blob \
  --out-dir build_O0_s256_full_o32
```

### 10.5 运行 runtime 功能一致性抽查

```bash
./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 32 \
  --mode hot4 \
  --helper-ops 4 \
  --threads 128

./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 256 \
  --mode full \
  --helper-ops 32 \
  --threads 128
```

### 10.6 核对点

复现后应重点核对：

1. `build/summary.csv` 中所有 case 的 `call_count` 是否仍为 `1`
2. `hot4` 下 `value_params` / `value_blob` 的 `registers` 是否随 `state_words` 明显上升，且 `s256` 仍可到 `255`
3. `hot4_dead` 下 `value_params` / `value_blob` 的 `registers` 是否回到接近 `10~13`
4. `vctx` 的 `stack_frame` 是否随 `state_words` 近似 `128/512/1024`
5. `s256/full/o32` 下：
   - `vctx` 是否仍有最重 local traffic
   - `value_params` 是否仍表现为 `spill=0` 但带有 `lmem`
   - `value_blob` 是否仍出现 stack/spill
6. `runtime_validate` 是否对 `s32/hot4/o4` 和 `s256/full/o32` 输出 `PASS`

### 10.7 常用排查命令

查看原始汇总：

```bash
sed -n '1,80p' build/summary.csv
sed -n '1,120p' build/REPORT.md
```

检查某个 case 的 PTX：

```bash
sed -n '1,120p' build/generated_ptx/value_blob_s256_full_o32.ptx
```

检查某个 case 的 SASS：

```bash
grep -n "CALL" build/sass/value_blob_s256_full_o32.sass
grep -nE '\\bLDL|\\bSTL' build/sass/value_blob_s256_full_o32.sass | head -n 40
```

## 11. 后续实验建议

若要继续回答“保留 call 边界时真实 ABI 成本”，下一轮实验应优先改微基准结构，而不是先改主线实现。建议优先尝试：

- 构造多个 caller 共享一个 helper，降低内联倾向
- 在 helper 中加入更复杂的控制流/循环，迫使后端保留 call
- 尝试把 micro-benchmark 接到真实 `sbt_ptx` 输出附近，而不是纯手写 helper
