# tools 说明

## 文件

- `run_experiment.py`
  - 命令行入口；
  - 重新导出 `BenchCase`、`render_ptx`、`parse_ptxas_info`；
  - 推荐用户直接运行这个文件。
- `experiment_lib.py`
  - 实验核心库；
  - 负责 PTX 生成、`ptxas`/`cuobjdump` 调用、指标解析与报告写出。

## 默认执行

```bash
python3 tools/run_experiment.py --out-dir build
```

## 关键参数

- `--arch`
  - 传给 `ptxas` 的目标架构，默认 `sm_89`
- `--state-words`
  - 状态规模，默认 `32 128 256 512 600`
- `--helper-ops`
  - 热状态在 helper 中的运算次数，默认 `4 32`
- `--abis`
  - `value_blob` / `pointer_store`
- `--scenarios`
  - `input_dead` / `passthrough_dead` / `ret_dead` / `live_subset`
- `--match`
  - 用 case 名子串过滤
- `--out-dir`
  - 输出目录

## 输出结构

- `generated_ptx/`
  - 每个 case 的 PTX 源文件
- `cubin/`
  - `ptxas` 输出
- `sass/`
  - `cuobjdump --dump-sass` 输出
- `call_windows/`
  - 每个 `CALL` 邻域窗口摘录
- `summary.csv`
  - 原始指标表
- `REPORT.md`
  - 自动生成的 Markdown 汇总

## 维护提示

- 如果新增场景，优先修改 `SCENARIOS` 与 `SCENARIO_DESCRIPTIONS`。
- 如果新增 ABI，必须同时补齐：
  - helper 声明/定义
  - kernel body
  - 指标解释
- 先跑：

```bash
python3 -m unittest tests.test_run_experiment -v
```

再跑单 case smoke：

```bash
python3 tools/run_experiment.py --match value_blob_input_dead_s32_o4 --out-dir build_smoke
```
