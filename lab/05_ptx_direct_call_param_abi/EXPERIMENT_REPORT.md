# PTX Direct Call 参数 ABI 实验报告

更新时间：`2026-03-14`

本文件是 `lab/05_ptx_direct_call_param_abi` 的单文件实验报告，目标是让读者不翻其它文档，也能快速了解：

- 为什么要做这组实验
- 实验怎么设计
- 关键指标怎么看
- 当前默认矩阵跑出来的主要结论是什么

本报告对应的本地默认实验产物由以下命令生成：

```bash
cd /work/ventus-env/sbtsim_lab/lab/05_ptx_direct_call_param_abi
./run_experiment.py --out-dir build
```

补充说明：

- `build/` 是本地生成目录，不纳入版本控制。
- 本报告里的数值结论以 2026-03-14 这次默认矩阵实跑结果为基础。
- 细表与完整图表见本地生成的 `build/summary.csv`、`build/REPORT.md`、`build/charts/*.svg`。

## 1. 背景

这个实验要回答的是：在 PTX direct call 场景里，跨 call 传递一大块状态时，应该继续沿用当前类似 `vctx` 的“上下文块搬运”思路，还是转向 `.param` value ABI。

主线争议点有三类：

1. `vctx`
   - PTX 文本通常较短，但 caller/callee 会显式做整块 local 上下文搬运。
2. `value_params`
   - 状态拆成很多离散 `.param .b32`。
   - 可能减少显式 local 搬运，但会把 ABI 表面做得非常宽。
3. `value_blob`
   - 状态仍然按 value ABI 传递，但 ABI 形状是单个聚合 blob，而不是几百个离散参数。

真正要看的不是“哪种写法看起来更优雅”，而是：

- 大状态下是否出现 `LDL/STL` 激增
- 是否把问题转成 `lmem`、`stack` 或 spill
- 当只有部分状态活跃时，后端是否能保住较低的 ABI 成本
- 聚合 blob 是否比离散参数更稳

## 2. 实验目标

当前版本重点回答四个问题：

1. 状态规模从 `256` 增长到 `512` 和 `600` 后，三种 ABI 的压力曲线如何变化。
2. callee 只使用 `25% / 50% / 75%` 状态时，value ABI 是否还能保持明显优势。
3. 在 `full` 最坏场景下，`value_blob` 是否仍然比 `value_params` 更值得继续。
4. `CALL` 边界是否被 `ptxas` 消掉，或者至少是否伴随明显 `LDL/STL`。

## 3. 实验设计

### 3.1 三种 ABI

- `vctx`
  - caller 先把状态写入 `.local vctx`
  - callee 通过 `vctx` 指针读回、修改、写回
  - caller 返回后再恢复状态并计算 checksum
- `value_params`
  - caller 把状态拆成离散 `.param .b32`
  - callee 读入后通过多 `.reg` 返回近似 value ABI
  - 注意：这组 case 仍会触发 `ptxas` 的 ABI disable warning，因此只能当探索性 proxy
- `value_blob`
  - caller/callee 通过单个 `.param .b8 blob[...]` 传递整块状态

### 3.2 默认矩阵

- `state_words`: `32`, `128`, `256`, `512`, `600`
- `use_mode`: `pct25`, `pct50`, `pct75`, `full`
- `helper_ops`: `4`, `32`
- `abi`: `vctx`, `value_params`, `value_blob`

总计：

- `5 x 4 x 2 x 3 = 120` 个 case

### 3.3 `use_mode` 含义

- `pct25`
  - callee 只真正计算前 `ceil(state_words * 25%)` 个状态
- `pct50`
  - callee 只真正计算前 `ceil(state_words * 50%)` 个状态
- `pct75`
  - callee 只真正计算前 `ceil(state_words * 75%)` 个状态
- `full`
  - callee 计算全部状态

这四种情节都保持：

- caller 对全部状态做 checksum
- 冷状态必须正确 passthrough

因此它们测的是“部分状态活跃，但全状态仍语义可观察”的真实 ABI 成本，不是 deadcode 情节。

## 4. 指标

实验关注五类关键指标：

- `registers`
  - 寄存器压力
- `ldl_stl_total`
  - 全 SASS 中 `LDL/STL` 总条数
- `call_window_ldl_stl`
  - `CALL` 邻域窗口内的 `LDL/STL`
- `local_footprint_bytes`
  - `lmem_bytes + stack_frame`
- `spill_total_bytes`
  - `spill_stores_total_bytes + spill_loads_total_bytes`

解读口径：

- `local_footprint_bytes` 更适合看“每线程 local 空间是否开始膨胀”
- `spill_total_bytes` 更适合看“高压区是否已经转成真实 spill”
- `ldl_stl_total` 更适合看整体 local traffic
- `call_window_ldl_stl` 更适合看 call 边界附近是否有明显 marshalling

## 5. 实验过程

每个 case 的微基准流程固定如下：

1. caller 初始化 `state_words` 个状态槽
2. 按某种 ABI 形式把状态传给 helper
3. helper 按 `use_mode` 只修改一部分或全部状态
4. caller 取回结果，对全状态做 checksum
5. 记录 PTX 规模、`ptxas -v` 信息和 SASS 指标

辅助检查：

- 当前 120 个 case 的 `call_count` 全部为 `1`
- 说明在这组默认 `-O3` 微基准下，`CALL` 没有被直接消掉

## 6. 代表性结果

下面列几组最有代表性的 case。

### 6.1 `s600 / pct25 / o4`

| ABI | regs | local_footprint | spill_total | LDL/STL | call-window | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx` | 255 | 4104 | 3312 | 1840 | 10 | 161050 |
| `value_params` | 255 | 1400 | 0 | 700 | 4 | 243825 |
| `value_blob` | 255 | 0 | 0 | 0 | 0 | 170125 |

结论：

- 当只使用 `25%` 状态时，`value_blob` 在 `600` 规模仍能保持零 local footprint、零 spill、零 `LDL/STL`
- `vctx` 即使只活跃 `25%` 状态，仍然有显著 local traffic
- `value_params` 比 `vctx` 轻，但 PTX 文本膨胀最明显

### 6.2 `s600 / pct50 / o4`

| ABI | regs | local_footprint | spill_total | LDL/STL | call-window | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx` | 255 | 3944 | 3440 | 1889 | 10 | 181054 |
| `value_params` | 255 | 1624 | 0 | 916 | 6 | 263829 |
| `value_blob` | 255 | 432 | 1288 | 322 | 6 | 190129 |

结论：

- `value_blob` 的拐点从这里开始出现
- 到 `pct50` 时，`value_blob` 已不再是零成本，但仍明显轻于 `vctx`
- `value_params` 没有显式 spill，但 `lmem` 继续上升

### 6.3 `s512 / pct50 / o32`

| ABI | regs | local_footprint | spill_total | LDL/STL | call-window | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx` | 255 | 3112 | 2104 | 1418 | 10 | 397956 |
| `value_params` | 255 | 1424 | 0 | 940 | 6 | 468587 |
| `value_blob` | 255 | 504 | 2168 | 542 | 6 | 405711 |

结论：

- helper 变长后，三种 ABI 都进入高压区
- `value_blob` 的 PTX 体积仍然接近 `vctx`
- `value_params` 继续承担最宽的 PTX 接口成本

### 6.4 `s600 / full / o32`

| ABI | regs | local_footprint | spill_total | LDL/STL | call-window | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `vctx` | 255 | 7680 | 85640 | 22434 | 10 | 798607 |
| `value_params` | 255 | 4824 | 0 | 21516 | 6 | 881382 |
| `value_blob` | 255 | 4832 | 86288 | 21572 | 6 | 807682 |

结论：

- `600/full/o32` 是当前默认矩阵的最坏区间
- 三种 ABI 都把寄存器顶到 `255`
- `vctx` 的 local footprint 最大，`value_params` 的 PTX 文本最大
- `value_blob` 在最坏场景下已不再明显优于 `value_params` 的动态成本，但文本体积仍接近 `vctx`

## 7. 总体结论

基于 2026-03-14 这次默认 120-case 实跑，可以归纳出 6 条结论。

### 7.1 `value_blob` 是当前最值得继续的方向

原因不是它在所有场景都最优，而是它在“轻到中等压力区”表现最稳：

- `256` 规模下，`pct25/pct50/pct75` 基本仍可维持零 local footprint、零 spill、零 `LDL/STL`
- `512/600` 且 `pct25` 时，仍然保持零 local/spill/traffic
- PTX 文本体积始终明显小于 `value_params`

### 7.2 `value_params` 更像探索性旁证，而不像主线候选

它的主要问题有两个：

- PTX 接口最宽，文本膨胀始终最明显
- `ptxas` 对这类多返回形状会给 ABI disable warning，因此测到的并不是干净的标准 direct-call value ABI

它仍然有参考价值，但不适合作为主结论核心依据。

### 7.3 `vctx` 的主要弱点是“冷状态也很难便宜”

从 `pct25` 开始就能看出：

- 即便只使用一小部分状态，`vctx` 仍然维持整块上下文搬运
- `512/600` 时很快出现明显 `stack`、spill 和 `LDL/STL`

也就是说，`vctx` 对“部分状态活跃”的适应性最差。

### 7.4 `value_blob` 的关键拐点在 `512/600` 与 `pct50` 之间

当前数据表明：

- `value_blob` 第一次出现明显 local/spill/`LDL/STL` 的代表点，是 `s512_pct50_o4`
- 这说明 `value_blob` 并不是“无条件免费”，它只是把高压区推迟了

这是后续主线实现需要重点关注的容量边界。

### 7.5 `full` 场景下三种方案都会进入高压区

在 `512/full`，尤其 `600/full/o32`：

- 三种 ABI 的 `registers` 都到 `255`
- `LDL/STL` 都已经非常高
- `value_blob` 与 `value_params` 的动态成本差距明显收窄

因此不能把 value ABI 理解成“full-state 下一定轻松胜出”。

### 7.6 `CALL` 没有被自动消掉

当前默认矩阵下：

- `call_count = 1` 覆盖全部 120 个 case

所以这轮实验的结论不是“后端自动把 helper 内联掉了”，而是“不同 ABI 形状在保留 call 边界时，代价差异非常明显”。

## 8. 建议读图顺序

如果只看本地 `build/charts/*.svg`，建议按这个顺序：

1. `local_footprint_bytes.svg`
2. `spill_total_bytes.svg`
3. `ldl_stl_total.svg`
4. `registers.svg`
5. `call_window_ldl_stl.svg`

重点观察：

- `value_blob` 从零成本区间转入高压区的拐点
- `vctx` 是否几乎从一开始就保持较高 local traffic
- `value_params` 是否在所有规模上都承担了更大的 PTX 体积

## 9. 限制与边界

这组实验故意只比较 PTX direct-call ABI 形状，不直接代表主线端到端收益。

它不能单独回答：

- 与真实 Ventus kernel 全流程是否完全等价
- 主线 `sbt/ptx_emit.cpp` 改造后能否一比一复制这些收益
- `value_params` 在标准 PTX ABI 约束下的真实上界

但它足够回答一个更现实的问题：

- 如果要继续推进 value ABI，优先级应该明显偏向 `value_blob`，而不是 `value_params`

## 10. 复现实验

默认矩阵：

```bash
./run_experiment.py --out-dir build
```

只看大规模高压区：

```bash
./run_experiment.py \
  --state-words 512 600 \
  --modes pct25 pct50 pct75 full \
  --helper-ops 4 32 \
  --out-dir build
```

历史兼容对照：

```bash
./run_experiment.py --modes hot4 hot4_dead --out-dir build_legacy
```

运行时一致性抽查：

```bash
./build_runtime_validate.sh

./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 600 \
  --mode pct50 \
  --helper-ops 4 \
  --threads 128
```
