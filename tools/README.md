# tools/

本目录用于放置工程工具、回归脚本与辅助程序，按“当前入口”与“归档”分层维护。

## 当前入口（active）

- `sbt_decode.cpp`：解码/pretty/CFG verify CLI（产物 `build/sbt_decode`）
- `sbt_ptx.cpp`：Ventus ELF -> PTX CLI（产物 `build/sbt_ptx`）
- `gen_spike_encoding_subset.cpp`：根据 `data/spike_want.txt` 生成 Spike 子集头
- `regext_bundle_test.cpp`：`regext/regexti` bundling 回归测试
- `regress.sh`：统一回归入口（聚合 smoke/gate/端到端；默认临时工作目录执行并自动清理，可用参数覆盖）
- `rodinia_ptx_smoke.sh`：Rodinia compile-first smoke（PTX + ptxas）
- `pds_ptx_smoke.sh`：PDS 参数与 PTX 映射 smoke
- `microtest_coverage_gate.sh`：Spike-vs-PTX 微测例 + 覆盖 gate 入口
- `check_spike_want_consistency.sh`：want 文件一致性 smoke
- `update_spike_want.py`：从 `VentusInst_basic.txt` 更新 `data/spike_want.txt`
- `ventus_inst_coverage.py`：指令覆盖统计
- `ventus_ocl_compare.py`：Spike/PTX 对照执行与结果比对
- `ventus_ocl_run.cpp`：OpenCL 微测例 runner（产物 `build/ventus_ocl_run`，可选）
- `ventus_regression_profile.py`：端到端回归耗时统计
- `ventus_pocl_fastpath.cpp`：PoCL 快路径 LD_PRELOAD（产物 `build/libventus_pocl_fastpath.so`）
- `cuda_trace.cpp`：CUDA Driver API trace LD_PRELOAD（产物 `build/libcuda_trace.so`，可选）

## 归档（archive）

- `archive/cu_hook.c`：早期临时 CUDA hook，当前无构建/文档入口
- `archive/ventus-env-ptx_driver-pds-params.patch`：历史补丁快照，保留追溯用途

## 维护约定

- 新增工具脚本必须遵循仓库根目录 `AGENTS.md` 中的 `tools/ 脚本约束`。
- 若工具不再作为当前入口，请移入 `tools/archive/` 并在本文件登记。
