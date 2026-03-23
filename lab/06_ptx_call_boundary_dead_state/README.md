# PTX Call Boundary Dead-State 实验

本目录已经实现并跑通一组“PTX call 边界死状态”微基准，目标是回答：

> 如果一批状态只在 PTX call/ret 的 ABI 边界出现，而不再进入真正可观察的 PTX use-def 链，`ptxas` 会不会把它们继续当成热 live state？

如果你只想快速知道本实验当前结论，先读：

- `EXPERIMENT_REPORT.md`

如果你要重跑并复核数据，先读：

- `tools/run_experiment.py`
- `tools/README.md`

## 1. 实验背景

`91bebb5` 之后，主线 `sbt/ptx_emit.cpp` 在 PTX direct call 边界采用了 `mutable_state_blob + machine_ctx_blob + runtime_env_blob` 的 value-blob 方案。

该方案修复了一批 correctness 问题，但也暴露出质量风险：

- PTX 文本明显膨胀；
- `ptxas` 编译时间显著上升；
- 寄存器压力、stack frame、spill 指标恶化；
- 已确认的重要根因之一，是某些本可保持冷态的状态被 emitter 过早 materialize 成 PTX 真实 live state。

关联文档：

- `../../doc/archive/PTX_XREG_QUALITY_REGRESSION_REPORT_2026-03-15.md`
- `../../doc/ventus-divergence-sgpr-analysis.md`
- `../05_ptx_direct_call_param_abi/README.md`

`lab/05_ptx_direct_call_param_abi` 已回答“当全状态仍语义可观察时，不同 call ABI 形状的成本如何”。但它没有单独回答更关键的一点：

- 如果冷状态只在 `.param blob` / call-boundary 处出现，后端能否把它们有效消去？

本实验就是把这个问题单独拿出来，用更干净的手写 PTX 微基准回答。

## 2. 实验目标

本实验聚焦四个问题：

1. `input_dead`
   - 全状态进入 call 输入 ABI；
   - callee 只使用热子集；
   - caller 返回后只观察热子集。
2. `passthrough_dead`
   - 冷状态形式上跨 call 输入并返回；
   - helper 内只做 ABI copy；
   - caller 不再观察冷子集。
3. `ret_dead`
   - callee 形式上返回整块状态；
   - caller 只读取热子集。
4. `live_subset`
   - 热子集真实参与计算；
   - 冷子集必须正确 passthrough；
   - caller 对全状态做校验。

这里要区分两件事：

- “状态被声明/打包进 ABI”
- “状态在 PTX 数据流中形成真实可观察 use”

只有把这两者拆开，才能判断 `value_blob` 的成本来自 ABI 本身，还是来自 emitter 制造的显式 live range。

## 3. 实验设计

### 3.1 ABI 对照

- `value_blob`
  - caller/callee 通过 `.param .b8 blob[...]` 传递整块状态；
  - 用来观察 blob ABI 在 dead-state 场景下是否仍被保守保留。
- `pointer_store`
  - caller 先把状态写入 `.local` backing store；
  - helper 通过指针读回/写回；
  - 作为“显式 materialized 内存状态”的对照组。

### 3.2 默认矩阵

- `state_words`: `32`, `128`, `256`, `512`, `600`
- `helper_ops`: `4`, `32`
- `hot_words`: `4`
- `abi`: `value_blob`, `pointer_store`
- `scenario`: `input_dead`, `passthrough_dead`, `ret_dead`, `live_subset`

总计：

- `5 x 2 x 2 x 4 = 80` 个 case

### 3.3 关键设计约束

- 不引入 `vbranch/join` 协议；
- 不引入 full-`x` broadcast；
- 不引入真实 Rodinia kernel；
- 只保留“call ABI 与 dead-state elimination”这一个变量。

## 4. 指标

默认采集：

- PTX 行数、字节数；
- `.param` 定义数；
- `ld.param` / `st.param`；
- `ld.local` / `st.local`；
- `ptxas -v` 的：
  - `registers`
  - `stack frame`
  - `spill stores`
  - `spill loads`
  - `lmem bytes`
- SASS 中的：
  - `CALL` 数
  - `PRET` 数
  - `LDL/STL` 总数
  - `CALL` 邻域窗口内的 `LDL/STL`

派生指标：

- `local_footprint_bytes = lmem_bytes + stack_frame`
- `spill_total_bytes = spill_stores_total_bytes + spill_loads_total_bytes`

## 5. 运行

在本目录执行：

```bash
python3 tools/run_experiment.py --out-dir build
```

常用命令：

```bash
# 只跑单个 case
python3 tools/run_experiment.py --match value_blob_input_dead_s512_o4 --out-dir build_smoke

# 只看高压规模
python3 tools/run_experiment.py \
  --state-words 512 600 \
  --helper-ops 4 32 \
  --out-dir build_high

# 只跑某一类场景
python3 tools/run_experiment.py \
  --scenarios input_dead passthrough_dead \
  --out-dir build_dead_only
```

依赖：

- `python3`
- `ptxas`
- `cuobjdump`
- NVIDIA GPU / CUDA 环境

本次报告使用环境：

- `ptxas`: CUDA 13.1 `V13.1.115`
- GPU: `NVIDIA GeForce RTX 4090`

## 6. 输出

脚本会生成：

- `build/generated_ptx/*.ptx`
- `build/cubin/*.cubin`
- `build/sass/*.sass`
- `build/call_windows/*.txt`
- `build/summary.csv`
- `build/REPORT.md`

说明：

- `build/` 是本地生成目录，不纳入版本控制。
- 需要重新运行脚本才能得到与当前环境一致的数字。
- 版本化结论以 `EXPERIMENT_REPORT.md` 为准，可复核原始数据以 `build/summary.csv` 为准。

## 7. 当前结论摘要

基于 `2026-03-15` 的默认 80-case 实跑：

1. 在 `input_dead`、`passthrough_dead`、`ret_dead` 中，`value_blob` 的 `registers/local/spill/LDL-STL` 基本保持常数级：
   - `registers = 8..13`
   - `local_footprint = 0`
   - `spill_total = 0`
   - `LDL/STL = 0`
2. 同样场景下，`pointer_store` 的 `local_footprint` 与 `LDL/STL` 随 `state_words` 线性增长。
3. 进入 `live_subset` 后，`value_blob` 不再“免费”：
   - `128` words 时寄存器已到 `135/136`
   - `256` words 起寄存器顶到 `255`
4. 因此，`ptxas` 对“只在 call 边界出现、之后不再观察”的 dead-state 有明显消除能力；
   但一旦状态在 PTX 层形成真实可观察 use，这种能力就不能再被假定。

## 8. 可复现检查

推荐的最小复现流程：

```bash
python3 -m unittest tests.test_run_experiment -v
python3 tools/run_experiment.py --out-dir build
```

验收标准：

- 单元测试通过；
- 80 个默认 case 全部编译完成；
- `build/summary.csv` 和 `build/REPORT.md` 成功生成；
- 与 `EXPERIMENT_REPORT.md` 的代表性数据趋势一致。
