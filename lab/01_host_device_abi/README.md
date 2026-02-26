# lab/01_host_device_abi: PTX backend ABI-compat PoC

## 1. 目标
本实验验证一种“**PTX 后端兼容 Ventus 现有 kernel ABI**”的做法：

- 上层（Ventus 现有软件栈/编译产物）继续按 Ventus 约定使用 `CSR_KNL` 指向 kernel metadata，并通过 metadata 的 `KNL_ARG_BASE` 访问 arg buffer。
- 后端不执行 RISC-V 指令流（不跑 `_start`/`crt0.S`），而是提供一个 NVIDIA PTX kernel，在 PTX 侧**模拟同样的 ABI 入口**，从 metadata/arg buffer 取出参数与 NDRange 配置，然后执行对应的 kernel 语义。

这一步的核心不是 vecadd 算法本身，而是：**如何把 Ventus 的“metadata + arg buffer + 32-bit 地址模型”映射到 CUDA/NVIDIA 的执行与地址模型**。

## 2. Ventus kernel ABI（本实验用到的最小子集）

### 2.1 `CSR_KNL` -> metadata
Ventus `crt0.S` 的关键逻辑是：读取 `CSR_KNL` 得到 metadata 地址，然后从 metadata 取出入口地址与参数区地址：

- 读取 metadata：见 [crt0.S:65](reference/crt0.S#L65)
- `KNL_ENTRY`：见 [crt0.S:66](reference/crt0.S#L66)
- `KNL_ARG_BASE`：见 [crt0.S:67](reference/crt0.S#L67)

metadata 的布局与字段偏移由 [ventus.h](reference/ventus.h) 定义（全是 32-bit 字段，单位字节偏移），本实验用到：
- `KNL_ARG_BASE` = 4
- `KNL_GL_SIZE_X` = 12
- `KNL_LC_SIZE_X` = 24
- `KNL_GL_OFFSET_X` = 36

### 2.2 arg buffer
`crt0.S` 将 `a0 = KNL_ARG_BASE` 后跳到 kernel entry。
在 `vecadd.dump` 中，kernel 函数 `vecadd` 入口处会从 `a0` 读取 3 个指针（arg0/arg1/arg2）：
- `lw t0, 8(a0)` / `lw t0, 4(a0)` / `lw t0, 0(a0)`：见 [vecadd.dump:75](testcase/vecadd.dump#L75) 到 [vecadd.dump:80](testcase/vecadd.dump#L80)

这意味着 arg buffer 的前 12 字节是：
- +0: `a_ptr`（u32）
- +4: `b_ptr`（u32）
- +8: `c_ptr`（u32）

## 3. 关键难点：Ventus 32-bit 地址 vs CUDA 64-bit 指针
Ventus kernel（以及其 ABI）在该实验里体现为“u32 指针”。但在 NVIDIA GPU 上：
- CUDA 全局内存指针是 64-bit
- PTX 的 `ld/st.global` 也基于 64-bit 地址

因此要做到“上层仍产出/传递 u32 指针”，PTX 后端必须提供一个**地址映射层**。

## 4. 本实验采用的 ABI 兼容实现方式（PoC）

### 4.1 单一 heap + 虚拟基址（VENTUS_BASE）
在 host 侧构造一个连续的 heap buffer，并把它作为“Ventus 虚拟地址空间”的承载。

- 固定 `VENTUS_BASE = 0x90000000`
- Ventus u32 地址 `p_u32` 解释为：
  - `offset = p_u32 - VENTUS_BASE`
  - `cuda_ptr = heap_base_u64 + offset`

这个映射在 PTX kernel 中实现（见 [vecadd.ptx](ptx/vecadd.ptx) 的注释和地址计算逻辑）。

这样，上层仍然可以使用 u32 指针（例如 `a_ptr_u32`），而 PTX 能把它转换成真正的 CUDA global 地址。

### 4.2 用 PTX kernel 模拟 `CSR_KNL` 入口
PTX kernel 入口：
- 文件： [vecadd.ptx](ptx/vecadd.ptx)
- kernel：`ventus_start`
- 参数：
  - `heap_base`（u64）：CUDA device pointer，指向 heap
  - `knl_addr`（u32）：metadata 的 Ventus 虚拟地址（等价于“CSR_KNL 的值”）

PTX kernel 内部做的事情（都是“ABI 兼容层”）：
1) `knl_addr(u32)` -> `knl_ptr(u64)`
2) 按 [ventus.h](reference/ventus.h) 的偏移读取 metadata 字段：
   - `KNL_ARG_BASE / KNL_GL_SIZE_X / KNL_LC_SIZE_X / KNL_GL_OFFSET_X`
3) 按 arg buffer 约定读取 `a/b/c` 三个 u32 指针
4) 用 `VENTUS_BASE` 映射成真实的 global 地址
5) 根据 NDRange 配置（`global_size_x`, `global_offset_x`）计算 work-item id

注意：本阶段 PoC **不需要**在 PTX 里解释 RISC-V 的 `csrr CSR_KNL` 指令；而是把 `CSR_KNL` 作为一个显式 PTX kernel 参数 `knl_addr` 传入。

### 4.3 NDRange / get_global_id(0) 的兼容点
Ventus 侧的 `get_global_id` 在 `vecadd.dump` 中有完整实现（包含 `setrpc/vbeq/vbne/join` 等 SIMT 控制指令），见 [vecadd.dump:98](testcase/vecadd.dump#L98) 开始。

在本 PoC 中，目标是 ABI 兼容而不是逐条指令翻译，所以采用等价语义：
- `linear_tid = ctaid.x * ntid.x + tid.x`
- 以 `KNL_GL_SIZE_X` 做边界保护（避免 launch 尺寸向上取整造成越界）
- `gid = linear_tid + KNL_GL_OFFSET_X`

metadata 中的 `KNL_LC_SIZE_X` 在本 PoC 中主要用于“保持 ABI 字段存在且数值一致”（由 runner 写入为 `blockDim.x`），而不是 PTX 计算 gid 必需。

## 5. Host 侧做了哪些事情（扮演 Ventus runtime）
本实验的 runner 用来“模拟 Ventus runtime/驱动”在启动 kernel 前会做的准备工作：

- 文件： [run_vecadd_ptx.cc](run_vecadd_ptx.cc)
- 关键职责：
  1) 在 host 端构造 heap（byte array）并按小端写入 u32/f32（保证与 Ventus 端内存视图一致）
  2) 在 heap 中布局：metadata、arg buffer、a/b/c 数组
  3) 把整个 heap 拷贝到 device
  4) 启动 `ventus_start(heap_base, knl_addr)`
  5) 回读 c 数组并校验

特别说明：这里的 `knl_addr`、`arg_base`、`a_addr` 等都是 **Ventus 虚拟地址（u32）**，通过 `VENTUS_BASE + offset` 构造。

## 6. 构建与运行
- Build 脚本： [build_vecadd.sh](build_vecadd.sh)
  - `nvcc -cubin` 把 PTX 编译成 cubin
  - `nvcc` 编译 runner（Driver API）
- Smoke 脚本： [smoke_vecadd.sh](smoke_vecadd.sh)

运行：

```bash
ARCH=sm_89 ./lab/01_host_device_abi/smoke_vecadd.sh
```

## 7. 与“最终目标：软件栈不改动支持后端”的关系
本 PoC 证明了：只要后端能提供以下能力，就可以让上层继续使用 Ventus ABI：

- 能接收/提供 `CSR_KNL` 所代表的 metadata 指针（u32），并按 [ventus.h](reference/ventus.h) 解释 metadata
- 能解释 arg buffer（u32 指针数组）
- 能支持 Ventus 的 32-bit 指针模型（通过 `VENTUS_BASE` 映射到真实后端地址空间）

目前 runner 是“人工替代”了 Ventus runtime 的那部分工作。要真正做到“不改软件栈”：
- 需要把这些“heap 构造 + metadata/arg buffer 填充 + launch”逻辑放入 Ventus 现有 runtime/driver 路径中
- 对于本仓库中参考实现，可对接方向是把 `pocl_ventus.cc` 类似的 metadata/参数准备流程接到 NVIDIA 后端（而不是 vt_* 仿真器），使上层 OpenCL 程序无需感知后端切换。

## 8. 当前 PoC 的刻意简化点（后续扩展方向）
- 只用到 metadata 的一小部分字段（X 维）；Y/Z 维、work_dim 的完整一致性还未覆盖
- 未实现 local memory/private memory、更多 CSR、print buffer 等
- 未做“逐条 Ventus 指令 -> PTX”的静态翻译；这里只验证 ABI + 等价语义执行路径
