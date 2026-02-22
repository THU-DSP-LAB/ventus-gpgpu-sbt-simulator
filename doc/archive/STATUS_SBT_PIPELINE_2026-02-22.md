# gpusim 现状更新与回归结果（2026-02-22）

本文记录相对 `doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md` 的**关键实现变更**与**验证结果**，用于快速定位“当前真相（as-built）”。

## 1. 关键变更：`s0(x8)` 的 bump 视为 frame 分配

### 1.1 背景

部分 kernel 的开头会出现 `addi s0, s0, imm`（或等价序列）对 `s0` 做常量调整。

此前实现曾尝试在 PTX prologue 中**识别并抵消**这个 bump（初始化 `s0 = (LDS_base - bump)`），以期让 kernel 执行 bump 后回到 `LDS_base`。

但该“抵消补偿”依赖编译器/PoCL 代码形态，且与 Ventus LLVM 测试中常见的 frame-pointer 风格（`s0 += frame_bytes` + 后续 `s0 - k`）并不总是兼容。

### 1.2 当前实现（as-built）

- `sbt/ptx_emit.cpp` 不再做 bump 识别/补偿。
- `x8(s0)` 仅按 `_start` ABI 初始化为：
  - `s0 = CSR_LDS + CSR_NUMW * 1024`
  - 在本后端中对应为：`shared_base_vaddr + warps_per_block*1024`
- kernel 自身若执行 `addi s0, s0, imm`，视为**frame 分配**，保持其语义，不在 prologue 中做额外“抵消”。

## 2. 回归验证（2026-02-22）

### 2.1 compile-first（PTX 可编译性）

Rodinia 11 个 kernel：`tools/rodinia_ptx_smoke.sh` 在 `ARCH=sm_75` 下通过（`sbt_ptx` 生成 PTX，`ptxas` 编译）。

### 2.2 端到端回归（PoCL/driver）

在 `VENTUS_BACKEND=ptx` 下：

- `tools/ventus_regression_profile.py --clean`：11 个 testcase 通过（用于统计 wall time 的脚本；已内置 `ventus-env/env.sh` 等价环境设置逻辑，可直接运行）。
- `ventus-env/regression-test.py`：11 个 testcase 通过（Ventus 工具链侧的回归入口；同样可直接运行，无需手工 `source env.sh`）。

## 3. 工具/环境：`ventus-env/regression-test.py` 不再依赖手工 `source env.sh`

### 3.1 现象与原因

在没有加载 `ventus-env/env.sh` 的环境中运行 `ventus-env/regression-test.py`，会出现：
- 编译期找不到 `CL/cl.h`（Rodinia Makefile 依赖 `OPENCL_INC/OPENCL_LIB` 与 `VENTUS_INSTALL_PREFIX`）。
- 运行期 `CL_PLATFORM_NOT_FOUND_KHR`（OpenCL ICD/PoCL 未正确配置）。

### 3.2 当前修复（as-built）

`ventus-env/regression-test.py` 已内置等价于 `ventus-env/env.sh` 的环境设置逻辑（自动补齐 `VENTUS_INSTALL_PREFIX`、`PATH`、`LD_LIBRARY_PATH`、`POCL_DEVICES`、`OCL_ICD_VENDORS`、`POCL_ENABLE_UNINIT`），因此可直接运行：

```bash
cd ventus-env
VENTUS_BACKEND=ptx python3 regression-test.py
```

注：若在非交互终端运行，`stty: Inappropriate ioctl for device` 可能出现（与被测程序的 TTY 检测相关），不影响回归结果判定。
