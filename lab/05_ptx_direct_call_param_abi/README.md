# PTX Direct Call 参数 ABI 微基准实验

本目录用于隔离比较三种 direct-call ABI 形状在 `ptxas`/SASS 下的成本：

- `vctx`
  - caller 先把状态写入 `.local vctx`，callee 通过指针读回/写回。
- `value_params`
  - caller 把状态拆成离散 `.param .b32` 传递，callee 用多 `.reg` 返回近似 value ABI。
- `value_blob`
  - caller/callee 通过单个聚合 `.param .b8 blob[...]` 传递整块状态。

如果想先快速了解实验背景、过程和当前结论，优先读：

- `EXPERIMENT_REPORT.md`

## 默认实验矩阵

- `state_words`: `32`, `128`, `256`, `512`, `600`
- `use_mode`:
  - `pct25`: callee 实际使用前 `25%` 状态，caller 仍校验全部状态
  - `pct50`: callee 实际使用前 `50%` 状态，caller 仍校验全部状态
  - `pct75`: callee 实际使用前 `75%` 状态，caller 仍校验全部状态
  - `full`: callee 使用全部状态，caller 校验全部状态
- `helper_ops`: `4`, `32`
- `abi`: `vctx`, `value_params`, `value_blob`

默认执行 `./run_experiment.py` 会生成 `5 x 4 x 2 x 3 = 120` 个 case。

补充兼容模式：

- `hot4`
  - 历史兼容模式；callee 只使用前 4 个状态，caller 仍校验全部状态
- `hot4_dead`
  - 历史 deadcode 对照；caller 只校验前 4 个状态

## 运行

在本目录执行：

```bash
./run_experiment.py
```

常用参数：

```bash
# 指定架构
./run_experiment.py --arch sm_89

# 指定部分 case
./run_experiment.py --match value_blob_s600_full_o32

# 只跑 512 / 600，关注高占用挡位
./run_experiment.py \
  --state-words 512 600 \
  --modes pct25 pct50 pct75 full \
  --helper-ops 4 32

# 跑历史 hot4_dead 对照
./run_experiment.py --modes hot4_dead --out-dir build_hot4_dead
```

依赖：

- `ptxas`
- `cuobjdump`
- `g++`
- CUDA Driver API 运行环境（仅 runtime validator 需要）

## 输出

脚本会把结果写到 `build/`：

- `build/generated_ptx/*.ptx`
- `build/cubin/*.cubin`
- `build/sass/*.sass`
- `build/call_windows/*.txt`
- `build/summary.csv`
- `build/REPORT.md`
- `build/charts/*.svg`
- `build/charts/INDEX.md`

说明：

- `build/` 是本地生成目录，不纳入版本控制。
- 需要重新执行 `./run_experiment.py` 才能得到与当前代码/报告一致的实验数据。
- `build/REPORT.md` 开头会解释 `state_words` / `use_mode` / `helper_ops` 的含义，并在每张图前附一行中文指标说明。

图表目前默认输出这些关键指标：

- `registers.svg`
- `ldl_stl_total.svg`
- `call_window_ldl_stl.svg`
- `local_footprint_bytes.svg`
- `spill_total_bytes.svg`

其中：

- `local_footprint_bytes = lmem_bytes + stack_frame`
- `spill_total_bytes = spill_stores_total_bytes + spill_loads_total_bytes`

图表采用“小 multiples”布局：

- 行：`helper_ops`
- 列：`use_mode`
- X 轴：`state_words`
- 曲线：三种 ABI

## 如何读结果

优先看 `build/REPORT.md` 和 `build/charts/*.svg`：

- `LDL/STL`
  - 看整体 local traffic 是否随着 ABI 形状或寄存器使用比例明显上升。
- `call_window_ldl_stl`
  - 看 `CALL` 邻域内是否出现明显 local-memory marshalling。
- `local_footprint_bytes`
  - 看 local 空间占用是否主要体现为 `stack_frame` 或 `lmem`。
- `spill_total_bytes`
  - 看大状态规模下是否开始转化成 spill。
- `registers`
  - 看 `value_params` / `value_blob` 是否因为高比例活跃状态把寄存器顶满。

推荐阅读顺序：

1. 固定 `helper_ops` 和 `use_mode`，横向比较三种 ABI。
2. 固定 `state_words`，比较 `pct25/pct50/pct75/full` 的拐点。
3. 最后再看 `512` 与 `600` 是否触发新的 `lmem`、spill 或 `LDL/STL` 上升。

## 运行时功能一致性验证

构建：

```bash
./build_runtime_validate.sh
```

运行示例：

```bash
./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 512 \
  --mode pct50 \
  --helper-ops 4 \
  --threads 128

./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 600 \
  --mode full \
  --helper-ops 32 \
  --threads 128
```

validator 默认对三种 ABI 分别与 CPU 参考实现比对，并做 ABI 之间的交叉比对。

## 当前文档边界

- 本目录的 `EXPERIMENT_REPORT.md` 现在只保留当前实验矩阵、指标定义和建议分析口径。
- 具体数值结果以每次执行后生成的 `build/REPORT.md`、`build/summary.csv`、`build/charts/*.svg` 为准。
