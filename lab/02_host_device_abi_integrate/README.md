# lab/02_host_device_abi_integrate：将仿真后端集成进 Ventus 工具链（PoCL 端到端）

> 注意：本 README 最初用于“vecadd-only 的 PTX wrapper”验证 host-device ABI。
> 当前仓库的 `ptx_device` 已升级为 **SBT PTX JIT**（driver 侧调用 `build/sbt_ptx` 动态翻译 ELF 函数并 JIT 运行），旧内容仍可作为 ABI 背景参考，但实现细节以代码为准。

## 目标

在不改动 PoCL 上层使用方式的前提下，把上一阶段验证过的“Ventus host-device ABI（CSR_KNL metadata + arg buffer）”接入到 `ventus-env` 的 driver 框架中，使得：

- PoCL Ventus device 仍然按原有路径生成 `object0.riscv`（RISC-V ELF）并构造 metadata/arg buffer；
- 通过 `VENTUS_BACKEND=ptx` 选择新后端；
- 直接运行 `ventus-env/pocl/build/examples/vecadd/vecadd total_threads block_threads` 得到正确结果（输出 `OK`）。

本阶段 driver 侧改动已经提交（历史参考）：
- 提交：`16772828338b7152d05b2c26a846dc8050c87ba4`

## 核心思路（从 vecadd-only 演进到 SBT PTX JIT）

- 新增一个 driver backend：`ptx_device`，实现 `vt_*` 接口，供 PoCL Ventus device 调用。
- `ptx_device` 在 `vt_start()` 时：
  - 读取 PoCL 已构造的 metadata/arg buffer（`metaDataBaseAddr` / CSR_KNL）；
  - 调用本仓库的 `build/sbt_ptx` 将 `object*.riscv` 中的目标 kernel 函数翻译为 PTX；
  - 通过 CUDA Driver API `cuModuleLoadDataEx` JIT 加载并 launch。

## 关键实现点（对应提交内容 + 现实现状）

1) 新增后端：`ptx_device`
- 新文件：`ventus-env/driver/driver/ptx_device/ventus.cpp`
  - 实现：`vt_dev_open/vt_dev_close/vt_dev_caps/vt_buf_alloc/vt_copy_to_dev/vt_copy_from_dev/vt_upload_kernel_file/vt_start/vt_ready_wait` 等。
  - 地址模型：PoCL 侧是 32-bit device ptr；driver 用 `0x90000000` 作为虚拟基址，把 device ptr 映射到一个 CUDA device heap 的 offset。
  - `vt_start()`：用 `vt_kernel_metadata_t` 的 `kernel_size[]` / `num_thread_local[]` 映射到 CUDA grid/block，并将 `metaDataBaseAddr`（Ventus u32 地址）作为 `knl_vaddr` 传给翻译后的 PTX。

2) 增加构建与安装
- 新文件：`ventus-env/driver/driver/ptx_device/CMakeLists.txt`
  - 生成并安装 `libptx_driver.so` 到 `ventus-env/install/lib/`。
  - 链接 CUDA Driver API（`libcuda.so`），并通过 `CUDAToolkit_INCLUDE_DIRS` 找到 `cuda.h`。

3) 接入 driver 总 CMake
- 修改：`ventus-env/driver/driver/CMakeLists.txt`
  - 增加 `DRIVER_ENABLE_PTX` 开关，并在开启时编译 `ptx_device`。

4) 接入 auto_select（通过环境变量选择后端）
- 修改：`ventus-env/driver/driver/auto_select/ventus.cpp`
  - 增加映射：`VENTUS_BACKEND=ptx` -> `libptx_driver.so`。

5) 接入 ventus-env 一键构建脚本
- 修改：`ventus-env/build-ventus.sh`
  - driver 构建 cmake 参数增加 `-DDRIVER_ENABLE_PTX=ON`。

## 运行方式

1. 构建并安装（只构建 driver 也可）：
- `bash ventus-env/build-ventus.sh --build "driver"`

2. 配置环境变量：
- `source ventus-env/env.sh`
- `export VENTUS_BACKEND=ptx`

3. 运行 vecadd（建议在该目录下运行，确保能找到相对路径的 `object0.riscv`）：
- `cd ventus-env/pocl/build/examples/vecadd`
- `./vecadd 128 64`

预期输出：`OK`

## 已知限制（原型期）

- `ptx_device` 走“结构化 CFG 验证 + 翻译 + JIT”，遇到不支持的指令/CFG 形态会 fail-fast。
- 暂不支持通过 `vt_copy_to_dev()` 直接写入“shared/local 段”（host 不可见）；若 PoCL 触发 `__local` 参数的 fallback 行为，可能会失败。
- 目前仍以“能 bring-up 运行 Rodinia/PoCL 示例”为主，未做位级一致性验证。

## 下一步（可选）

- 扩展到更多 OpenCL kernel：
  - 在 `vt_start()` 根据 `kernel_name` 做 dispatch，加载/选择不同 PTX wrapper。
- 若要接近“真实 Ventus 执行”：
  - 需要实现 Ventus ISA 到 PTX 的翻译或一个可解释执行的后端，并复用 PoCL 的 ELF 装载/metadata 传递路径。
