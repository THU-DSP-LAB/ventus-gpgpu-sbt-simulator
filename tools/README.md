# tools/

本目录用于放置工程工具、回归脚本与辅助程序，按“当前入口”与“归档”分层维护。

## 当前入口（active）

- `sbt_decode.cpp`：解码/pretty/CFG verify CLI（产物 `build/sbt_decode`）
- `sbt_ptx.cpp`：Ventus ELF -> PTX CLI（产物 `build/sbt_ptx`）
- `gen_spike_encoding_subset.cpp`：根据 `data/spike_want.txt` 生成 Spike 子集头
- `regext_bundle_test.cpp`：`regext/regexti` bundling 回归测试
- `custom_decode_test.cpp`：repository-local custom non-MMA decode 回归测试
- `instruction_metadata_contract_test.cpp`：shared instruction metadata / Spike want 同步 / CFG build+verify+direct-call control semantics authority / fail-fast 合同测试，并覆盖 poisoned non-`ret` `jalr` 仍被记为 `unsupported_jalr`
- `external_mnemonic_contract_test.cpp`：pretty / JSON / coverage / builtin symbol 等 external mnemonic contract 回归测试
- `custom_ptx_emit_test.cpp`：custom non-MMA PTX lowering + ptxas compile-first 回归测试
- `ptx_emit_call_prototype_test.cpp`：PTX helper 前向调用 prototype 回归测试
- `ptx_emit_leader_lane_abi_test.cpp`：replicated scalar-state / value ABI / divergence 回归测试（文件名沿用历史命名）
- `check_ptx_emit_name_allowlist.py`：完整 post-split PTX emitter 文件集与 `ptx_emit_internal.hpp` 中剩余 `name` 读取 allowlist 静态检查，并验证 builtin public allowlist / lookup table / control dispatch 同步
- `custom_non_mma_oracle.py`：custom non-MMA 的 Spike-vs-PTX 对照 + compile-first gate
  - current：默认先调用 `ventus_feature_probe.py` 检测 Shuffle / VCVT / packed / SFU 在所选 `--env-sh` 对应的 `VENTUS_INSTALL_PREFIX/bin/clang` 与同一 ventus root 的 `spike` 中是否可用；不可用的 feature 会逐 kernel 显式 `SKIP`，可用 feature 继续真实执行
  - 可传 `--no-auto-skip-features` 关闭 feature skip，此时缺失工具链能力会按原始执行路径显式失败
  - shuffle 类 kernel 会自动提升到 32-lane warp 规模执行，避免 `n < 32` 时的伪失败
  - 若当前 custom kernel 产物包含连续 `regext/regexti` 前缀，可显式传 `--spike-compat-nested-regext`；该开关只作用于本 gate 内部调用的 `sbt_decode/sbt_ptx` 与 PTX backend 路径，不改变工具默认 fail-fast 语义
- `custom_mma_oracle.py`：custom MMA 的分阶段 gate（compile-first / full）
  - current：默认先调用 `ventus_feature_probe.py` 检测 MMA 在所选 `--env-sh` 对应的 `VENTUS_INSTALL_PREFIX/bin/clang` 与同一 ventus root 的 `spike` 中是否可用；不可用时显式 `SKIP custom MMA oracle gate`
  - 可传 `--no-auto-skip-features` 关闭 feature skip，此时缺失工具链能力会按原始执行路径显式失败
  - `--stage compile-first`：先通过一次 Spike 运行 materialize 当前 kernel 的 `object0.riscv`，再执行 `sbt_decode --require-known` 与 `sbt_ptx + ptxas`
  - `--stage full`：在 compile-first 基础上增加 semantic compare；所有 current supported MMA family 都走 `Spike / sbtsim PTX / CPU reference` 三方对照
  - current：gate 会按目标 feature macro 单独 materialize 源文件，只暴露一个 MMA kernel，绕开 Ventus PoCL 在多-kernel OpenCL 源文件上可能误选首个入口的运行时缺陷
  - current：默认覆盖较小/较大两档随机样本；逐 kernel 报告 `PASS` / `BLOCK` / `FAIL`，non-current MMA family 继续显式 `BLOCK`
- `custom_non_mma_specs.py`：custom non-MMA oracle 的 kernel 清单与比较模式数据
- `ventus_feature_probe.py`：custom feature 能力探针
  - current：检测 MMA / Shuffle / SFU / VCVT / packed custom 指令，分别验证 clang builtin、Spike `DECLARE_INSN`/insn 文件，以及 MMA/SFU 所需 `dependencies/mma-sim` / `dependencies/unfu`
  - `--summary` 打印完整人类可读报告；`--format shell` 可给外层脚本消费
- `mma_cpu_ref.py`：当前 MMA 共享 CPU reference helper
  - 覆盖当前已支持的 8 条 MMA family，提供随机 seed 生成、CPU 参考计算与 `fp16` / `f32` 容差比较
- `fp16_mma_spike_cpu_ref.py`：`fp16 -> fp16` `m16n8k16 row.col` / `m16n16k16 row.col` 的 Spike-vs-CPU-reference 独立测例
  - 输入由 host 随机 seed 驱动，再在 kernel 内映射到有限 `fp16` 值集合，避免把 NaN/Inf payload 选择混进测例主结论
  - current：该脚本通过共享 `mma_cpu_ref.py` 维护 `fp16 -> fp16` 两条 family 的 CPU reference 与 Spike 语义，不直接替代统一 MMA semantic gate
  - 比较规则：`NaN` 按分类相等，非 `NaN` half lane 默认要求 `<= 1 ULP`
- `regress.sh`：统一回归入口（聚合 smoke/gate/端到端；默认临时工作目录执行并自动清理，可用参数覆盖）
  - current：默认 `--mma-stage=full`，确保统一回归默认覆盖当前已支持 MMA family 的 compile-first 与三方 semantic gate
  - 端到端阶段会强制使用当前树的 `build/sbt_ptx` 作为 `GPU_SBT_PTX`
  - `--mma-stage` 控制 `custom_mma_oracle.py` 阶段，默认 `full`
  - `all` preset 等价于 `quick + e2e`；其中 custom non-MMA oracle 会串行执行 36 个 OpenCL kernel 的 Spike/PTX/compile-first 链路，属于分钟级 gate
  - current：每个 step 在独立 process group 中执行并打印耗时；收到 `INT/TERM` 时会终止当前 step 的子进程树，避免中断后遗留 `ventus_ocl_run` / compiler 进程
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
