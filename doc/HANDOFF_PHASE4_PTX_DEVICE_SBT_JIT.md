# 阶段4交接：PoCL/driver 集成（`ptx_device` + SBT PTX JIT）

本文用于**交接当前实现进度**与**代码结构/功能说明**，便于在环境问题修复后由他人继续推进 Rodinia 端到端 bring-up。

> 关键现状：本仓库已经把 `ventus-env/driver/driver/ptx_device` 从“vecadd-only 的 PTX wrapper”升级为**按 kernel ELF 动态调用 `build/sbt_ptx` 翻译 PTX 并用 CUDA Driver API JIT+launch**。

---

## 1. 目标与范围

**目标（阶段4）**
- PoCL 仍按原路径生成 `object*.riscv`（RISC-V ELF）+ metadata/arg buffer；
- 通过 `VENTUS_BACKEND=ptx` 选择 `ptx_device`；
- `ptx_device` 在 `vt_start()` 里根据 `kernel_name`：
  1) 用 `vt_upload_kernel_file()` 已上传的 ELF 作为输入；
  2) 调用 `build/sbt_ptx` 生成 PTX；
  3) 用 CUDA Driver API `cuModuleLoadDataEx` JIT 加载并 launch；
  4) 逐步跑通 `ventus-env/rodinia/opencl/*/*.riscv`。

**显式非目标（原型期）**
- 不支持不可结构化 CFG 的兜底（例如 software SIMT stack/PC dispatch）。
- 不追求位级完全一致：先 bring-up 可运行，再逐步补齐语义与指令覆盖。
- 暂不覆盖所有指令/系统调用/原子/一致性等复杂语义。

---

## 2. 核心语义约束（已固化）

### 2.1 默认不翻译 `_start` 是什么意思？还能跑吗？

- Ventus ELF 的 `_start` 是“启动/运行时入口”，包含初始化、`jalr` 间接跳转等形态；原型期不打算翻译 `_start`。
- 当前执行路径是：PoCL/driver 直接以 `kernel_name`（例如 `BFS_1`、`vecadd`）为入口翻译该函数的 `.text` 范围，**不是从 `_start` 走到 kernel**。
- PTX 入口 prologue 会按 ABI 需要初始化必要状态（如 CSR_KNL 的 metadata 指针、arg buffer base 等）。
- 因此：**不翻译 `_start` 仍可以运行**（前提是 kernel CFG/指令满足当前翻译器约束）。

### 2.2 地址空间（Ventus 32-bit 数值地址 → CUDA backing）

当前 driver 与 PTX emitter 使用一致的数值地址区间（原型默认）：
- `0x7000_0000 ~ 0x7fff_ffff`：shared/local 段（PTX dynamic shared，host 不可见）
- `0x8000_0000 ~ 0x8fff_ffff`：ELF PT_LOAD backing（`elf_base`，global）
- `>= 0x9000_0000`：heap backing（`heap_base`，global，PoCL buffer/argbuf/private 等）

driver 侧实现：`vt_copy_to_dev/from_dev` 与 `vt_upload_kernel_file` 会按上述区间路由到对应 backing。

---

## 3. 代码结构与关键实现点

### 3.1 SBT PTX 生成器（本仓库）

- CLI：`tools/sbt_ptx.cpp`
  - 用法：`./build/sbt_ptx <elf> --func <kernel> --out <ptx> --sm <cc> --encoding-h <path> --require-known`
  - 先做 `.text` slice → decode → CFG build → CFG verify（`setrpc/vbranch/join/barrier/jalr` 检查）→ emit PTX
  - 提示：dynamic shared 需求为 `warps*1024(wctx) + warps*1024(stack) + ldsSize`（`tools/sbt_ptx.cpp:228`）

- PTX emitter：`sbt/ptx_emit.cpp`
  - PTX kernel param ABI 固化为：
    - `(.u64 elf_base, .u64 heap_base, .u32 knl_vaddr)`
  - prologue 中建立：
    - `__sbt_shmem` dynamic shared 基址
    - 每 warp `WarpCtx`（1024B/warp）与 scalar stack（1024B/warp）
    - `lds_ptr = shmem_base + warps_per_block*1024`（注意：这只偏移了 wctx，未包含 stack；因此 driver 侧给的 dynamic shared 必须覆盖 wctx+stack+lds，且 emitter 侧也需保证访问不越界）
  - **Ventus vbranch 操作数顺序（关键细节）**
    - Ventus `vbranch` 家族（`vbeq/vbne/vblt/vbge/vbltu/vbgeu`）的“编码字段（rs1/rs2）”与“语义/objdump 打印顺序”是交换的：
      - `llvm-objdump`/cyclesim 日志以 `vb* v<rs2>, v<rs1>, off` 展示；
      - 因此 emitter 侧在生成 predicate 时必须按 `v(rs2) op v(rs1)` 解释，否则常见 bounds-check 会反向，导致线程大面积提前跳转（Rodinia BFS 曾因此 `gpu_results` 全 `-1`）。
    - 当前实现选择在 `sbt/ptx_emit.cpp` 修正该顺序（而不是改 decoder），以保持 decode 阶段尽量“按位还原”，语义差异集中在 emitter。

> 备注：当前 emitter 的 shared 布局属于原型期约定；若后续要精确区分 wctx/stack/lds 三段，建议把 `lds_ptr` 改为 `shmem_base + warps*1024(wctx) + warps*1024(stack)`，并同步更新 driver 的计算逻辑。

### 3.2 `ptx_device`（Ventus driver backend）

文件：`ventus-env/driver/driver/ptx_device/ventus.cpp`

#### 3.2.1 设备资源与 backing 分配
- `vt_dev_open()`
  - `cuInit`/primary ctx
  - 分配：
    - `elf_base`：大小固定 `256MiB`（覆盖 `0x8000_0000..0x9000_0000`）
    - `heap_base`：默认 `1024MiB`（可用 `VENTUS_PTX_HEAP_MB` 或 `VENTUS_HEAP_MB` 调整；会 clamp 到不超过 `0x1000_0000_0 - 0x9000_0000`）
  - `sm` 选择策略：
    - 默认读到 device compute capability（例如 4090 是 `sm_89`）
    - **原型期默认 clamp 到 `<=75`**，避免 `.version 7.0` + `sm_89` 的 CUDA JIT 兼容性问题
    - 可用 `VENTUS_PTX_SM`（或 `GPU_SBT_SM`）强制目标 SM

#### 3.2.2 ELF 装载（PT_LOAD）
- `vt_upload_kernel_file()`
  - `get_data_from_elf()` 取 PT_LOAD blocks
  - 按 block `vaddr` 区间写入：
    - `0x8000_0000..` → `elf_base + (vaddr-0x8000_0000)`
    - `0x9000_0000..` → `heap_base + (vaddr-0x9000_0000)`
  - 同时记录“本次上传的 ELF 路径”，供 `vt_start()` 选择输入：
    - `elf_path_by_kernel_id[kernelID] = <path>`
    - `last_elf_path = <path>`（PoCL 目前常用 `kernel_id=0`/`kernelID=0`，用 last_elf 兜底）
  - 若 ELF 里存在 `vaddr>=0x9000_0000` 的段，会 bump `next_vaddr`，避免后续 `vt_buf_alloc()` 覆盖它。

#### 3.2.3 运行时翻译 + JIT + 缓存
- `get_or_jit_kernel()`（内部 helper）
  - cache key：`"<elf>|<kernel>|sm=<cc>"`
  - 记录 `elf` 的 mtime；mtime 变化会使缓存失效并重新翻译
  - 生成的 PTX 默认放到：`/tmp/ventus_sbt_ptx/<kernel>.sm<cc>.<hash>.ptx`
  - 调用 `sbt_ptx` 的命令行（可在 debug 日志中看到）：
    - `sbt_ptx <elf> --func <kernel> --out <ptx> --sm <cc> --encoding-h <encoding.h> --require-known`
  - JIT：`cuModuleLoadDataEx`，开启 error/info log buffer；设置 `VENTUS_PTX_JIT_INFO=1` 可打印 info log

#### 3.2.4 launch ABI + dynamic shared
- `vt_start()`
  - 取 `kernel = metaData->kernel_name`
  - 找到对应 ELF 路径（优先按 `kernel_id`，否则用 `last_elf_path`）
  - `get_or_jit_kernel()` 得到 `CUfunction`
  - kernel params 固定为：
    - `CUdeviceptr elf_base`
    - `CUdeviceptr heap_base`
    - `uint32_t knl_vaddr = (uint32_t)metaData->metaDataBaseAddr`
  - dynamic shared：
    - `warps = metaData->wg_size`（若为 0 则按 threads/32 推导）
    - `shmem = warps*1024(wctx) + warps*1024(stack) + align(ldsSize,16)`
    - 若 `shmem > 48KiB`，尝试 `cuFuncSetAttribute(CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, shmem)`（best-effort）
  - grid/block：
    - `grid = metaData->kernel_size[]`
    - `block = metaData->num_thread_local[]`

#### 3.2.5 设备内存 API
- `vt_buf_alloc()`：只在 heap 区间 bump 分配（`>=0x9000_0000`），并对分配区间做 `cuMemsetD8` 清零
- `vt_buf_free()`：仅支持 **LIFO** free（否则 warning 并忽略）
- `vt_copy_to_dev/from_dev()`：按 vaddr 区间路由到 `elf_base/heap_base`

### 3.3 driver 构建约束

文件：`ventus-env/driver/driver/ptx_device/CMakeLists.txt`
- 显式 `target_compile_features(... cxx_std_20)`
- 链接 CUDA Driver API（`-lcuda`）与 `spdlog/fmt`

---

## 4. 环境变量与可调开关

### 4.1 翻译/缓存相关
- `GPU_SBT_PTX` / `VENTUS_SBT_PTX`：指定 `sbt_ptx` 可执行文件路径（默认尝试 `<repo>/build/sbt_ptx`，否则用 `sbt_ptx` in PATH）
- `GPU_SBT_ENCODING_H`：指定 `encoding.h` 路径（默认 `<repo>/ventus-env/spike/riscv/encoding.h`）
- `GPU_SBT_PTX_CACHE_DIR`：PTX 输出缓存目录（默认 `/tmp/ventus_sbt_ptx`）
- `GPU_SBT_PTX_NO_COMMENTS=1`：生成 PTX 不带注释（减小体积/便于 diff）

### 4.2 CUDA/目标架构相关
- `VENTUS_PTX_SM`：强制 PTX `.target sm_XX`（默认 clamp 到 `sm_75`）
- `VENTUS_PTX_HEAP_MB` / `VENTUS_HEAP_MB`：heap 大小（MiB，默认 1024）
- `VENTUS_PTX_JIT_INFO=1`：打印 CUDA JIT 的 info log

---

## 5. 构建与运行（建议在“本机正常终端”执行）

> 说明：在 Codex 沙箱内可能因为 `/dev/nvidia*` 设备节点权限不足导致 `cuInit` 报错；请在真实终端/正确用户权限下执行。

### 5.1 构建本仓库工具
```bash
cmake -S . -B build
cmake --build build -j
```
> 若 CMake 报 `CMakeCache.txt directory ... is different than ... where CMakeCache.txt was created`，
> 说明 `build/` 目录来自其他路径（例如拷贝/移动 repo 后遗留）。直接删除或重命名 `build/` 后重新运行上面的两条命令即可。

### 5.2 构建并安装 ventus driver（包含 `libptx_driver.so`）
```bash
bash ventus-env/build-ventus.sh --build "driver"
```

### 5.3 运行 PoCL vecadd（最小端到端）
```bash
source ventus-env/env.sh
export VENTUS_BACKEND=ptx

# 原型期建议强制 sm_75
export VENTUS_PTX_SM=75

# 注意：`pocl` 的 vecadd 示例会在运行时查找 `vecadd.cl`（或 .spir/.poclbin）。
# 在某些 build 目录下可能只有临时生成的 `object0.cl` 而没有 `vecadd.cl`，会报：
#   Can't find vecadd.cl SPIR / PoCLbin file ...
# 稳妥做法：在源码目录运行（用 build 产物的可执行文件）：
cd ventus-env/pocl/examples/vecadd
../../build/examples/vecadd/vecadd 128 64
```

### 5.4 运行 Rodinia（示例：bfs）
```bash
source ventus-env/env.sh
export VENTUS_BACKEND=ptx
export VENTUS_PTX_SM=75

cd ventus-env/rodinia/opencl/bfs
./run
```

---

## 6. 已知问题与下一步 TODO

### 6.1 CUDA Driver API 权限问题（环境）
- 若出现 `cuInit failed: CUDA_ERROR_OPERATING_SYSTEM`：
  - 通常是当前进程对 `/dev/nvidiactl`、`/dev/nvidia0`、`/dev/nvidia-uvm` 缺少 **读写**权限（只读也可能失败）。
  - 需要在真实环境中确保用户属于正确的组（例如 `video`/`render`，依发行版而定），或在容器/沙箱里正确映射并授权设备节点。

### 6.2 PTX `.version` / `.target` 兼容性
- 之前在 `sm_89` 上 JIT 报错：`PTX .version 7.0 does not support .target sm_89`。
- 当前 driver 默认把目标 SM clamp 到 `sm_75`，并允许用 `VENTUS_PTX_SM` 覆写。
- 后续更优方案：
  - 让 emitter 输出更高 PTX version（例如 8.x）以支持更高 `.target`；
  - 或保持 `.target sm_75` 但在更高代 GPU 上运行（可行但不充分利用新特性）。

### 6.3 shared/local 段与 `__local` 参数
- 当前 driver 不支持 host 直接 copy 到 shared/local 段（`0x7000_0000..`）。
- PoCL 侧对 `__local` 参数有一些 fallback/临时行为（见 `ventus-env/pocl/lib/CL/devices/ventus/pocl_ventus.cc` 中 TODO），在某些 benchmark 可能触发不兼容；需要逐个 benchmark bring-up。

### 6.4 指令覆盖与 CFG 结构化失败
- 任何 `sbt_ptx` 的 decode/CFG verify/emit 失败都会导致 `vt_start()` fail-fast。
- bring-up 策略：
  1) 先挑无 barrier/分支简单的 kernel（如 `vecadd`，再到部分 Rodinia kernel）
  2) 收集失败的 `unsupported.inst` / `invalid.cfg` / `barrier` 报错
  3) 在 `sbt/riscv_decode.*` / `sbt/cfg_verify.*` / `sbt/ptx_emit.*` 逐条补齐

### 6.5 shared 布局一致性
- 当前 emitter 的 `lds_ptr` 偏移只按 `warps*1024` 计算；driver 侧按 `wctx+stack+lds` 给足 shared 可以避免越界，但布局语义不够“自洽”。
- 建议后续把 emitter 的 shared 布局显式三段化（wctx/stack/lds），并在输出 PTX 中使用一致的 base 偏移。

### 6.6 Rodinia BFS 输出全 -1（已修复，2026-02-15）
- 现象：`ventus-env/rodinia/opencl/bfs/./run` 能跑完，但 `gpu_results` 除起点外几乎全为 `-1`（表现为 kernel 基本“全跳过”）。
- 根因：Ventus `vbranch`（`vbeq/vbne/vblt/vbge/vbltu/vbgeu`）的**寄存器编码顺序与语义/objdump 打印顺序是交换的**：
  - `llvm-objdump`/cyclesim 日志以 `vb* v<rs2>, v<rs1>, off` 的顺序展示；
  - 旧的 PTX emitter 按 `rs1 op rs2` 生成 predicate，导致如 bounds-check 类条件反向，线程大面积提前跳转到 `join`。
- 修复：在 `sbt/ptx_emit.cpp` 的 vbranch 翻译中交换操作数（用 `rs2` 作为左操作数，`rs1` 作为右操作数）。
- 验证：
  - `tools/rodinia_ptx_smoke.sh` 仍然全部通过（`ptxas -arch=sm_75`）。
  - `ventus-env/rodinia/opencl/bfs/./run` 输出 `--cambine:passed:-)`，且 `cpu_results` 与 `gpu_results` 匹配。

### 6.7 翻译缓存失效提示（实践）
- `ptx_device` 的 PTX cache key 里包含 `elf|kernel|sm` 并用 ELF 的 mtime 判断是否需要重新翻译；
  - **修改 `sbt_ptx`/emitter 代码后**，仅靠运行同一个 ELF 可能仍复用旧 PTX。
- bring-up 时建议：
  - 用 `GPU_SBT_PTX_CACHE_DIR=/tmp/ventus_sbt_ptx_<tag>` 指向一个新目录来强制重新生成；或
  - 手动清理默认缓存目录 `/tmp/ventus_sbt_ptx`。

---

## 7. 快速定位（实用）

- 翻译输出（默认缓存）：`/tmp/ventus_sbt_ptx/*.ptx`
- 打开 JIT info：
  - `export VENTUS_PTX_JIT_INFO=1`
- 强制 SM：
  - `export VENTUS_PTX_SM=75`
- 如需把 `sbt_ptx` 路径固定到本仓库：
  - `export GPU_SBT_PTX=$PWD/build/sbt_ptx`
- 快速检查“能否翻译 + ptxas 能否编译”（不代表数值正确）：
  - `bash tools/rodinia_ptx_smoke.sh`

---

## 8. 本次实验记录（2026-02-15）

- 本仓库 `build/` 目录的 `CMakeCache.txt` 曾来自不同路径（例如 `/home/wangyh/gpusim`），需要重建（重建后 `build/sbt_ptx` 可用）。
- `tools/rodinia_ptx_smoke.sh`（Rodinia 若干 `.riscv` + kernel 名）在 `-arch=sm_75` 下全部通过。
- PoCL `vecadd` 在 `ventus-env/pocl/examples/vecadd` 目录运行可通过（见 5.3 的更新写法）。
- Rodinia `bfs` 曾出现 `gpu_results` 全 `-1`，已由 6.6 的 vbranch 操作数交换修复。
