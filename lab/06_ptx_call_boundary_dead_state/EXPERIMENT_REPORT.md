# PTX Call Boundary Dead-State 实验报告

更新时间：`2026-03-15`

本报告对应的本地默认实验产物由以下命令生成：

```bash
REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT/lab/06_ptx_call_boundary_dead_state"
python3 tools/run_experiment.py --out-dir build
```

本次运行环境：

- `ptxas`: CUDA 13.1 `V13.1.115`
- GPU: `NVIDIA GeForce RTX 4090`
- 目标架构：`sm_89`

## 1. 实验问题

本实验要回答的不是“Ventus machine state 是否需要跨 PTX call 传递”，而是更细的一层：

> 当某些状态只在 PTX call/ret ABI 的边界中出现，而不再进入真正可观察的 PTX use-def 链时，`ptxas` 是否会把这些状态继续保守地当成热 live state？

如果答案是“会消去”，那就说明：

- `value_blob` 的问题未必来自 blob ABI 形状本身；
- 更可能来自 emitter 把冷状态过早 materialize 为显式 use。

如果答案是“不会消去”，那就说明：

- 不能指望“把冷热管理交给 `ptxas`”。

## 2. 实验设计

### 2.1 ABI

- `value_blob`
  - 用 `.param .b8 blob[...]` 传整块状态。
- `pointer_store`
  - caller 先把状态写到 `.local` backing store；
  - helper 通过 `.u64` 指针读写。

### 2.2 场景

- `input_dead`
  - 全状态只体现在输入 ABI 中；
  - caller 与 callee 最终只观察前 4 个热字。
- `passthrough_dead`
  - 冷状态形式上输入并返回；
  - helper 对冷状态只做 ABI copy；
  - caller 不再观察冷状态。
- `ret_dead`
  - helper 形式上返回整块状态；
  - caller 只读取热子集。
- `live_subset`
  - 热状态真实参与计算；
  - 冷状态必须正确 passthrough；
  - caller 对全状态做校验。

### 2.3 默认矩阵

- `state_words`: `32`, `128`, `256`, `512`, `600`
- `helper_ops`: `4`, `32`
- `hot_words`: `4`
- 总计 `80` 个 case

## 3. 核心结果

### 3.1 `input_dead`

| case | regs | local_footprint | spill_total | LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `pointer_store_input_dead_s600_o32` | 24 | 2400 | 0 | 302 | 59029 |
| `value_blob_input_dead_s600_o32` | 13 | 0 | 0 | 0 | 61331 |

结论：

- 当冷状态只出现在输入 ABI 时，`value_blob` 没有把它们保留成真实资源成本。
- 对照组 `pointer_store` 则稳定承担整块 backing store 的 local 代价。

### 3.2 `passthrough_dead`

| case | regs | local_footprint | spill_total | LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `pointer_store_passthrough_dead_s600_o32` | 24 | 4800 | 0 | 1498 | 100588 |
| `value_blob_passthrough_dead_s600_o32` | 13 | 0 | 0 | 0 | 106998 |

结论：

- 冷状态即使形式上“经过一次输入再返回”，只要它们不再被 caller 观察，`value_blob` 仍表现为近似零动态成本。
- 这说明 call-boundary copy 本身并没有迫使 `ptxas` 把冷状态变成真实 `LDL/STL` 或 local footprint。

### 3.3 `ret_dead`

| case | regs | local_footprint | spill_total | LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `pointer_store_ret_dead_s600_o32` | 24 | 2400 | 0 | 302 | 54871 |
| `value_blob_ret_dead_s600_o32` | 8 | 0 | 0 | 0 | 57114 |

结论：

- 仅仅“helper 形式上返回整块状态”并不会让 `value_blob` 自动变重；
- 只要 caller 不读取冷返回值，这部分返回开销就能被后端压到极低。

### 3.4 `live_subset`

| case | regs | local_footprint | spill_total | LDL/STL | PTX bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `pointer_store_live_subset_s600_o32` | 36 | 4800 | 0 | 1796 | 137253 |
| `value_blob_live_subset_s128_o32` | 136 | 0 | 0 | 0 | 34665 |
| `value_blob_live_subset_s256_o32` | 255 | 0 | 0 | 0 | 64519 |
| `value_blob_live_subset_s600_o32` | 255 | 0 | 0 | 0 | 146047 |

结论：

- 一旦 caller 需要对全状态做校验，`value_blob` 不再是“交给后端就会免费优化”的方案。
- 它在 `128` words 时已经显著升高寄存器压力，在 `256` words 起直接顶到 `255` registers。
- 这和前三类 dead-state 场景形成鲜明对照。

## 4. 汇总结论

本实验支持下面这个判断：

1. `ptxas` 对“只在 call-boundary ABI 出现、之后不再被观察”的死状态，确实有很强的消除能力。
2. 这种能力在本实验的 `value_blob` case 中相当稳定：
   - `input_dead` / `passthrough_dead` / `ret_dead`
   - `32 -> 600` words
   - `helper_ops = 4 / 32`
   - 都保持 `local_footprint = 0`、`spill_total = 0`、`LDL/STL = 0`
3. 但这并不意味着可以把一般性的状态冷热管理都交给后端：
   - 只要状态在 PTX 层形成真实可观察 use，`value_blob` 仍会迅速推高寄存器压力。
4. 因而，对主线 `xreg/call ABI` 的启示不是“blob 一定没问题”，而是：
   - **ABI-only dead state 可以被很好消去；**
   - **eager materialization 出来的显式 live range 才是更危险的部分。**

## 5. 额外观察

- 所有 80 个 case 的 `call_count` 都是 `1`。
- 也就是说，这次结论不是靠 helper 被直接消掉得到的，而是在保留 call 边界的前提下得到的。
- `pointer_store` 的 `CALL` 邻域窗口中始终能看到明显 `LDL/STL`；
  `value_blob` 对应窗口则基本没有 local traffic。

## 6. 复现步骤

```bash
REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT/lab/06_ptx_call_boundary_dead_state"
python3 -m unittest tests.test_run_experiment -v
python3 tools/run_experiment.py --out-dir build
```

复现后重点检查：

- `build/summary.csv`
- `build/REPORT.md`
- `build/call_windows/pointer_store_passthrough_dead_s600_o4.call0.txt`
- `build/call_windows/value_blob_passthrough_dead_s600_o4.call0.txt`

只要趋势与本报告一致，本实验就算复核成功。
