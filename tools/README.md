# tools/

本目录用于放置工程工具、回归脚本与辅助程序，按“当前入口”与“归档”分层维护。

## 当前入口（active）

- `sbt_decode.cpp`：解码/pretty/CFG verify CLI（产物 `build/sbt_decode`）
- `sbt_ptx.cpp`：Ventus ELF -> PTX CLI（产物 `build/sbt_ptx`）
- `gen_spike_encoding_subset.cpp`：根据 `data/spike_want.txt` 生成 Spike 子集头
- `regext_bundle_test.cpp`：`regext/regexti` bundling 回归测试
- `custom_decode_test.cpp`：repository-local custom non-MMA decode 回归测试
- `custom_ptx_emit_test.cpp`：custom non-MMA PTX lowering + ptxas compile-first 回归测试
- `ptx_emit_call_prototype_test.cpp`：PTX helper 前向调用 prototype 回归测试
- `ptx_emit_leader_lane_abi_test.cpp`：replicated scalar-state / value ABI / divergence 回归测试（文件名沿用历史命名）
- `custom_non_mma_oracle.py`：custom non-MMA 的 Spike-vs-PTX 对照 + compile-first gate
  - shuffle 类 kernel 会自动提升到 32-lane warp 规模执行，避免 `n < 32` 时的伪失败
  - 若当前 custom kernel 产物包含连续 `regext/regexti` 前缀，可显式传 `--spike-compat-nested-regext`；该开关只作用于本 gate 内部调用的 `sbt_decode/sbt_ptx` 与 PTX backend 路径，不改变工具默认 fail-fast 语义
- `regress.sh`：统一回归入口（聚合 smoke/gate/端到端；默认临时工作目录执行并自动清理，可用参数覆盖）
  - 端到端阶段会强制使用当前树的 `build/sbt_ptx` 作为 `GPU_SBT_PTX`
- `rodinia_ptx_smoke.sh`：Rodinia compile-first smoke（PTX + ptxas）
- `pds_ptx_smoke.sh`：PDS 参数、single-Global entry ABI 与 PTX 映射 smoke
- `microtest_coverage_gate.sh`：Spike-vs-PTX 微测例 + 覆盖 gate 入口
- `check_spike_want_consistency.sh`：want 文件一致性 smoke
- `update_spike_want.py`：从 `VentusInst_basic.txt` 更新 `data/spike_want.txt`
- `ventus_inst_coverage.py`：指令覆盖统计
- `ventus_ocl_compare.py`：Spike/PTX 对照执行与结果比对
  - 若未显式设置 `GPU_SBT_PTX`，检测到 `build/sbt_ptx` 时会自动绑定并打印所选路径
- `ventus_ocl_run.cpp`：OpenCL 微测例 runner（产物 `build/ventus_ocl_run`，可选）
  - 支持 `--in <raw-u32.bin>` 覆盖输入 buffer，供 packed microtest 直接消费原始 bit pattern
- `ventus_regression_profile.py`：端到端回归耗时统计
  - 若未显式设置 `GPU_SBT_PTX`，检测到 `build/sbt_ptx` 时会自动绑定并打印所选路径
- `ventus_pocl_fastpath.cpp`：PoCL 快路径 LD_PRELOAD（产物 `build/libventus_pocl_fastpath.so`）
- `cuda_trace.cpp`：CUDA Driver API trace LD_PRELOAD（产物 `build/libcuda_trace.so`，可选）

## 归档（archive）

- `archive/cu_hook.c`：早期临时 CUDA hook，当前无构建/文档入口
- `archive/ventus-env-ptx_driver-pds-params.patch`：历史补丁快照，保留追溯用途

## 维护约定

- 新增工具脚本必须遵循仓库根目录 `AGENTS.md` 中的 `tools/ 脚本约束`。
- 若工具不再作为当前入口，请移入 `tools/archive/` 并在本文件登记。
