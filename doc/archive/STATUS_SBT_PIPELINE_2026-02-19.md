# gpusim 当前现状与 SBT（Ventus ELF → PTX）流程梳理（2026-02-19）

> 更新提示：若你在排查“最新实现现状/回归是否通过”，请优先阅读 `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`（包含 2026-02-22 的实现变更与回归结果）。

本文将当前仓库的**实现现状**、**OpenSpec 分阶段方案**、以及**静态二进制翻译管线（ELF→decode→CFG verify→PTX）**按“代码真实入口”落盘，便于后续 bring-up 与排错。

> 说明：
> - 本文基于仓库现有代码与文档整理，不引入新设计。
> - 当前环境可用 CUDA 工具链（`ptxas/nvcc`）且可访问 GPU（`/dev/nvidia*` 存在），因此本文包含了 2026-02-19 的端到端实测结果（PoCL + `VENTUS_BACKEND=ptx`）。

---

## 1. 项目目标与当前状态（事实）

- 目标：将 Ventus ISA（RISC‑V RV32 + V 语义重解释 + 自定义 SIMT 指令）做静态二进制翻译（SBT）为 NVIDIA PTX，以便在 NVIDIA GPU 上做更快的功能级验证（见 `README.md`）。
- 语言/构建：C++20 + CMake（见 `CMakeLists.txt`）。
- 核心库：`sbt_lib`（ELF 读取、Spike 编码表解析、解码、CFG、CFG verify、PTX emitter）。
- 主要 CLI：
  - 解码/CFG 验证：`build/sbt_decode`（源码 `tools/sbt_decode.cpp`）
  - PTX 生成器：`build/sbt_ptx`（源码 `tools/sbt_ptx.cpp`）
- compile-first smoke（Rodinia 11 个 kernel）：`tools/rodinia_ptx_smoke.sh`

### 1.1 当前环境的工具链观测

- `ptxas --version`：CUDA 13.1（V13.1.115）
- `nvcc --version`：CUDA 13.1（V13.1.115）
- `nvidia-smi`：Driver 590.48.01；GPU：NVIDIA GeForce RTX 4090（Compute Capability 8.9）

### 1.2 GPU 可用性快速验证（2026-02-19）

- PTX JIT（Driver API）加载验证：对 `build/sbt_ptx` 生成的 `/tmp/BFS_1.ptx`（kernel `BFS_1`）执行 `cuModuleLoadDataEx + cuModuleGetFunction` 成功。

---

## 2. OpenSpec 中“新更改”的分阶段实现方案

> 当前环境未安装 `openspec` CLI（`openspec: command not found`），但 change 文档均在仓库 `openspec/changes/` 中可直接阅读。

### 2.1 `add-simple-ptx-prototype`：最小可运行原型（simple testcase）

路径：
- `openspec/changes/add-simple-ptx-prototype/proposal.md`
- `openspec/changes/add-simple-ptx-prototype/design.md`
- `openspec/changes/add-simple-ptx-prototype/tasks.md`

核心要点：
- 用 `testcases/simple` 验证最小闭环（向量按 lane 执行、单 warp 启动）。
- `vid.v` → PTX `%laneid`；访存直接 `ld/st.global`。
- 不依赖固定设备虚拟地址（如 `0x90000000`），改为 kernel param 传入基址指针。

### 2.2 `add-sbt-rodinia-bringup`：Rodinia bring-up 的分阶段 gate（ELF → decode/CFG → PTX）

路径：
- `openspec/changes/add-sbt-rodinia-bringup/proposal.md`
- `openspec/changes/add-sbt-rodinia-bringup/design.md`
- `openspec/changes/add-sbt-rodinia-bringup/tasks.md`

固化的阶段拆分（与现有代码结构一一对应）：
- Stage 1：ELF/`.text` 提取 + 解码（含 `regext` 前缀合并）
- Stage 2：CFG 构建 + `setrpc/vbranch/join` 结构化验证 + `barrier` 合法性检查
- Stage 3：PTX 生成（compile-first）+ `ptxas` 可编译
- Stage 4（可选）：PoCL/driver 端到端（SBT PTX JIT）

---

## 3. 代码结构总览（入口与职责）

### 3.1 `sbt/`：SBT 核心实现

- ELF 读取：`sbt/elf_reader.cpp`（读 `.text`、`.symtab`）
- Spike 编码表解析：`sbt/spike_encoding_parser.cpp`（解析 `ventus-env/spike/riscv/encoding.h`）
- 指令解码：`sbt/riscv_decode.cpp`
  - 标量 RV32 子集：按 opcode/funct3/funct7 解码
  - Ventus 扩展：通过 match/mask 命中后按指令名分类填充操作数/立即数
  - `regext`：前缀只作用**下一条**指令，扩展寄存器编号到 0..255
- CFG 构建：`sbt/cfg.cpp`（切基本块、建边）
- CFG verify：`sbt/cfg_verify.cpp`
  - 解析 `setrpc` 的 join PC（支持 `auipc + setrpc` 形态）
  - 对 `vbranch` 做结构化校验（postdom/side-exit 等；multi-entry 目前仅记录不作为 fail-fast 条件）
  - `barrier` 收敛性检查（保守：不能落在不可证明收敛的分支区域）
  - 非标准 `jalr`（非 ret）记录为 unsupported（fail-fast）
- PTX emitter：`sbt/ptx_emit.cpp`

### 3.2 `tools/`：CLI 与脚本

- `tools/sbt_decode.cpp` → `build/sbt_decode`
  - `funcs`：列 `.symtab` 函数符号
  - `verify`：校验 ELF `.text` 字节与 `.dump` 一致
  - `pretty`：类 objdump 输出（便于人工对照）
  - `cfgverify`：对函数做 CFG verify 并输出 JSON
- `tools/sbt_ptx.cpp` → `build/sbt_ptx`
  - `--func <kernel>`：按函数符号切片 `.text`，decode→CFG→verify→emit PTX
- `tools/rodinia_ptx_smoke.sh`
  - 为 11 个 Rodinia kernel 生成 PTX 并用 `ptxas` 编译（compile-first）

---

## 4. 静态二进制翻译管线：Ventus ELF → PTX（按真实调用链）

本节描述的是当前 `sbt_ptx` 的实际流水线（与 OpenSpec Stage 1/2/3 对齐）。

### 4.1 Stage 1：ELF 读取 + 解码

1) 从 ELF 读取：
   - `.text`：指令字节序列 + vaddr
   - `.symtab`：函数符号（用于定位 kernel 入口与函数边界）
2) 建立 pattern 表：
   - 从 Spike 的 `ventus-env/spike/riscv/encoding.h` 解析 `DECLARE_INSN()` 对应 `MATCH_*/MASK_*`
   - **仅保留 bring-up whitelist 指令名**
3) 解码 `.text` slice：
   - 以 4 字节为粒度读 u32 指令字
   - 若遇 `regext` 前缀：记录扩展字段并跳过输出（仅作用下一条）
   - 对下一条指令：先尝试 pattern match（Ventus 扩展），否则走 RV32 标量 decode
   - `--require-known` 时，任何 `unknown` 直接报错退出（fail-fast）

### 4.2 Stage 2：CFG build + structural verify（`setrpc/vbranch/join/barrier/jalr`）

1) CFG build：
   - 按 terminator/target 切 leader，构建基本块与显式边
2) structural verify：
   - `setrpc`：静态解析 join PC（当前实现为回看近处 `auipc` 写同一寄存器）
   - `vbranch`：要求 join PC 对应 `join` 且成为基本块入口；检查 postdom/side-exit；multi-entry 目前仅记录不作为 fail-fast 条件
   - `barrier`：保守检查，要求 barrier block 不落在任何“不可证明收敛”的 vbranch region 内
   - `jalr`：仅允许标准 ret 形态（否则记录为 unsupported 并拒绝）

不通过则 `sbt_ptx` 直接退出（fail-fast），避免生成“可能错误”的 PTX。

### 4.3 Stage 3：PTX emit（compile-first）

PTX emitter 的关键约定：

#### 4.3.1 Kernel 参数 ABI（固定 3 个）

`sbt_ptx` 生成的 PTX kernel 入口固定为：
- `elf_base: .u64`：映射 Ventus 数值地址 `[0x8000_0000, 0x9000_0000)` 的 backing
- `heap_base: .u64`：映射 `>=0x9000_0000` 的 backing
- `knl_vaddr: .u32`：Ventus 数值地址（metadata buffer base，对应 `CSR_KNL`）

#### 4.3.2 标量（warp-uniform）语义：active-lane leader + WarpCtx

PTX 采用“active-lane leader”执行标量副作用，并将标量寄存器文件存放在 per-warp shared 区域，保证在发散路径不会出现标量状态分裂。

实现表现为：
- 每个基本块开头计算 `activemask` 与 leader predicate
- leader 做 `ld/st.shared` 更新标量寄存器状态
- `bar.warp.sync` 同步

#### 4.3.3 地址空间方案：Ventus 数值地址区间分流（方案 B）

按 `doc/ventus-isa/others.md` 的区间约定：
- `0x7000_0000..0x7fff_ffff` → shared（dynamic shared）
- `0x8000_0000..0x8fff_ffff` → ELF backing（`elf_base + (addr-0x8000_0000)`）
- `>=0x9000_0000` → heap backing（`heap_base + (addr-0x9000_0000)`）

#### 4.3.4 SIMT 控制流：结构化翻译 + NVIDIA 硬件分歧/收敛

- `setrpc/join`：在结构化方案中作为 no-op（语义用于 verify 与结构化恢复）
- `vbranch`：生成 PTX predicate + `@p bra`，依赖硬件 divergence/reconvergence
  - 注意：Ventus `vbranch` 的寄存器操作数顺序与标量分支不同；当前 emitter 已在 vbranch lowering 处做交换（见 `sbt/ptx_emit.cpp` 中相关注释）。
- `barrier`：翻译为 `bar.sync 0;`，并依赖 Stage 2 的 barrier 合法性检查避免发散路径 barrier。

#### 4.3.5 私有内存索引访存：`vlw.v/vsw.v`

`vlw.v/vsw.v` 是 Ventus 的“GPU-private-memory indexed ops”（Spike 语义依赖 `CSR_PDS/CSR_NUMW/CSR_NUMT/CSR_TID`），不同于普通 `vlw12.v/vsw12.v` 的数值地址空间访存。

**更新（2026-02-19）：**当前 PoCL/driver 端到端路径只传入 64B `CSR_KNL` metadata buffer（见 `KNL_MAX_METADATA_SIZE=64`），其中不包含 `CSR_PDS`/`pdsBaseAddr`，因此 SBT PTX 侧无法从 `knl_vaddr` 推导出正确的 PDS base。

现阶段为避免“把 private spill 写进 heap/覆盖输入 buffer”（已在 Rodinia `nn` 中复现），emitter 将 `vlw.v/vsw.v` 临时改为使用 **PTX local memory**（per-thread）模拟 private memory backing（行为更接近“真正的 per-thread private”，但与 Spike 的 global-backing PDS 分配方式并不完全等价，且容量有固定上限）。

---

## 5. Stage 4（可选）：PoCL/driver 端到端（SBT PTX JIT）

交接文档：`doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`

实现位置（注意：位于 `ventus-env/`，仓库约束是“不要修改 ventus-env 中的文件”，除非做工具链集成且需征得同意）：
- `ventus-env/driver/driver/ptx_device/ventus.cpp`

端到端逻辑概述：
- PoCL 上传 ELF（PT_LOAD 段）到 driver 的 `elf_base/heap_base` backing
- `vt_start()`：
  1) 根据 `kernel_name` 找到对应 ELF
  2) 调 `sbt_ptx` 生成 PTX
  3) CUDA Driver API JIT：`cuModuleLoadDataEx` + `cuModuleGetFunction`
  4) 计算动态 shared：`warps*(wctx+stack) + ldsSize` 并 launch

关键环境变量（driver）：
- `VENTUS_PTX_SM` / `GPU_SBT_SM`：目标 SM
- `VENTUS_PTX_HEAP_MB` / `VENTUS_HEAP_MB`：heap 大小
- `GPU_SBT_PTX`：指定 `sbt_ptx` 路径
- `GPU_SBT_ENCODING_H`：指定 Spike `encoding.h`
- `GPU_SBT_PTX_CACHE_DIR` / `GPU_SBT_PTX_NO_COMMENTS`
- `VENTUS_PTX_JIT_INFO`：打印 JIT info log

### 5.1 端到端实测结果（PoCL + `VENTUS_BACKEND=ptx`，2026-02-19）

本次实测统一使用：
- `VENTUS_BACKEND=ptx`
- `VENTUS_PTX_SM=75`（可选；不设置时 driver 会按设备 CC 推导并 clamp 到 `sm_75`）
- 其余 `GPU_SBT_*` 环境变量均为可选（driver 内部会自动定位本仓库路径）：
  - `GPU_SBT_PTX`：默认使用 `${gpusim_root}/build/sbt_ptx`（若使用 `build-gpu/`，可显式指定为 `${gpusim_root}/build-gpu/sbt_ptx`）
  - `GPU_SBT_ENCODING_H`：默认使用 `${gpusim_root}/ventus-env/spike/riscv/encoding.h`
  - `GPU_SBT_PTX_CACHE_DIR`：默认 `/tmp/ventus_sbt_ptx`（driver 每次 JIT 都会重新调用 `sbt_ptx` 覆盖生成的 `.ptx`，因此无需为“刷新”单独换目录）
  - `GPU_SBT_PTX_NO_COMMENTS=1`：可选（用于减少生成 PTX 体积/日志噪音）

运行结果（均来自 `ventus-env/rodinia/opencl/*/run` 或 `ventus-env/pocl/examples/*`；日志保存在 `/tmp/gpusim_*_run_*.log`）：

| Benchmark | 规模/参数（见对应 `run` 脚本） | 结果 | 现象/错误摘要 | rc | 日志 |
|---|---|---|---|---:|---|
| PoCL `vecadd` | `vecadd 128 64` | 通过 | 输出 `OK` | 0 | `/tmp/gpusim_vecadd_run_20260219_0317.log` |
| PoCL `trig` | 固定 `N=8`（源码 `trig.c`），无参数 | 通过 | 输出 `OK` | 0 | `/tmp/gpusim_pocl_trig_run_20260219_034138.log` |
| PoCL `example0` | 无参数（kernel=`integer_mad`） | 通过 | 输出 `PASS` | 0 | `/tmp/gpusim_pocl_example0_run_20260219_034211.log` |
| PoCL `example1` | 无参数（kernel=`dot_product`） | 通过 | 输出 `OK` | 0 | `/tmp/gpusim_pocl_example1_run_20260219_034212.log` |
| PoCL `example2a` | 固定 `global=[2*256,4096/32]=[512,128]`，`local=[64,1]`（源码 `example2a.c`） | 通过 | 输出 `OK` | 0 | `/tmp/gpusim_pocl_example2a_run_20260219_041241.log` |
| Rodinia `bfs` | `${DATA_DIR}/bfs/graph1k.txt` | 通过 | 输出 `--cambine:passed:-)` | 0 | `/tmp/gpusim_bfs_run_20260219_0317.log` |
| Rodinia `gaussian` | `-f ../../data/gaussian/matrix16.txt -v`（16×16） | 通过 | 输出 `CPU & OpenCL results match, OK!` | 0 | `/tmp/gpusim_gaussian_run_20260219_0301.log` |
| Rodinia `nn` | `list1k.txt -r 20 -lat 13 -lng 27 --ref nvidia-result-1k-lat13-lng27` | 通过 | 输出 `Testcase results match ... OK` | 0 | `/tmp/gpusim_nn_run_20260219_0317.log` |
| Rodinia `kmeans` | `-i ../../data/kmeans/512_34f.txt -g nvidia_result_512_34f_k5` | 通过 | 输出 `DUT result matches ... OK` | 0 | `/tmp/gpusim_kmeans_run_20260219_0320.log` |
| Rodinia `backprop` | `-n 1024 --ref nvidia-result-n1024` | 通过 | 输出 `All weights match ... OK` | 0 | `/tmp/gpusim_backprop_run_20260219_0314.log` |
| Rodinia `b+tree` | `mil.txt + command_512.txt --ref output_512.nvidia.txt` | 通过 | 输出 `Validation succeeded!` | 0 | `/tmp/gpusim_btree_run_20260219_0325.log` |

备注：
- 多数用例会出现 `vt_buf_free non-LIFO (ignored)` 警告；本次实测中其不影响用例是否通过，但应视为 driver allocator 行为与 PoCL 释放顺序不匹配的信号。
- `Rodinia nn/backprop` 的修复点（均在本仓库 `sbt/` 内，不涉及修改 `ventus-env/`）：
  - 修正 `vfmadd.vv` 的 PTX lowering：按 Spike 语义实现 `vd = (vd * vs1) + vs2`。
  - 修正 `vlw.v/vsw.v`：在缺少 `CSR_PDS`/PDS base 的情况下，改为用 PTX local memory 模拟 per-thread private backing，避免错误写入 heap 污染输入数据。
  - 补齐 `_start` ABI 初始化：补充 `s0(x8)=CSR_LDS+CSR_NUMW*1024`，并修正 `CSR_LDS` 返回值为 numeric shared base（否则 backprop 会 `CUDA_ERROR_ILLEGAL_ADDRESS`）。
- `Rodinia kmeans/b+tree` 的修复点：
  - 修正比较类指令（如 `vmslt{,u}`/`vmflt`）的结果表示为 **0/1**（而不是 `0xffffffff/0`），避免 `vxor.vi 1` 等布尔逻辑与后续等值比较失真。
- `PoCL trig/example2a` 的新增覆盖点（均在本仓库 `sbt/` / `tools/` 内）：
  - 新增指令覆盖：`vmulh.vx`、`vand.vi`、`vadd12.vi`，以及 `regexti` 前缀（扩展寄存器 + 扩展 5-bit immediate）。
  - 新增 builtin call lowering：`_Z3{cos,sin,tan}Dv4_f`、`_Z4{fabs,sqrt}Dv4_f`（float4），以及 `_Z5mad24iii`（int）。
  - （历史实现记录）曾针对“kernel 入口处 bump `s0(x8)`”的模式在 PTX prologue 中做过模式匹配并初始化 `x8 = CSR_LDS+CSR_NUMW*1024-bump`；该补偿已在 2026-02-22 起移除，当前改为把 bump 视为 frame 分配（见 `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`）。

---

## 6. 为什么“当前只能正常运行少数几个测例”（基于代码现状的主要原因）

按阻塞性排序（越靠前越可能导致“直接跑不起来/必错”）：

1) **PoCL `__local` 参数支持不完整**
   - `ventus-env/pocl/lib/CL/devices/ventus/pocl_ventus.cc` 存在明确告警/占位逻辑：`"not support local buffer arg yet."`
   - 这通常会直接限制可端到端通过的 OpenCL kernel 集合（即使 PTX 能生成且能 `ptxas` 编译）。

2) **Ventus 扩展指令匹配采用 bring-up whitelist 子集**
   - whitelist 定义在 `tools/sbt_ptx.cpp` 与 `tools/sbt_decode.cpp` 内。
   - 超出 whitelist 的 Ventus 扩展指令会解码为 `unknown`，在 `--require-known` 下直接 fail-fast。

3) **builtin call 支持范围有限**
   - emitter 对 call 的支持是硬编码白名单；测例若依赖其它 builtin/math/helper，翻译期会拒绝或行为缺失。

4) **PTX `.version/.target` 兼容性与 SM clamp 策略**
   - emitter 输出 `.version 7.0`；在高代 GPU 上常需要通过 driver clamp SM 或提升 PTX version 才能稳定 JIT。

---

## 7. 下一步修复建议（不改代码，仅列优先级路线）

### 7.1 优先修复“跑不起来”的阻塞点

- 把 PoCL `__local` 参数打通到“Ventus shared 数值地址空间（`0x7000_0000..`）↔ dynamic shared (LDS 区域)”的闭环：
  - 先用最小 kernel 验证 local arg 的指针/布局/生命周期，再扩展到 Rodinia。

### 7.2 再处理“能跑但结果不对/偶发挂死”

- 将失败分为四类并收集最小诊断信息：
  1) 翻译期 fail-fast（unknown/unsupported/cfgverify 不通过）
  2) JIT/launch 失败（PTX version/target/shmem 超限）
  3) 运行挂死（通常是 barrier/线程提前退出/收敛假设不成立）
  4) 运行完成但结果错误（指令语义/地址路由/ABI 不一致）
- 对每个失败 kernel：
  - 先用 `sbt_decode pretty/cfgverify` 与 `sbt_ptx` 把问题定位到“第一处不满足约束的点/第一条可疑指令”。
  - 再用端到端日志（driver 的 sbt_ptx log、JIT info log）确认是翻译问题还是环境/JIT 问题。

### 7.3 扩大可翻译指令覆盖面（中期）

- 以 `.dump` 为输入统计缺口（whitelist 外 Ventus 扩展指令），按频次与 bring-up 优先级扩展：
  - pattern（encoding.h）
  - decode 分类与 operand/imm 抽取
  - emitter lowering 与必要的语义约束
- 同时扩 builtin call 覆盖范围（优先覆盖 PoCL/Rodinia 常见 builtin）。

---

## 8. 常用命令清单（bring-up/排错）

构建：
```bash
cmake -S . -B build
cmake --build build -j

# 若 build/ 目录的 CMakeCache 来源路径不一致（例如曾在别的源码目录生成过），可使用新目录：
cmake -S . -B build-gpu
cmake --build build-gpu -j
```

解码/对照：
```bash
./build/sbt_decode verify ventus-env/rodinia/opencl/bfs/object0.riscv
./build/sbt_decode funcs  ventus-env/rodinia/opencl/bfs/object0.riscv
./build/sbt_decode pretty ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known
```

CFG verify（输出 JSON）：
```bash
./build/sbt_decode cfgverify ventus-env/rodinia/opencl/b+tree/object0.riscv --func findRangeK --require-known --verbose --json /tmp/findRangeK.cfg.json
```

生成 PTX + `ptxas` 编译：
```bash
./build/sbt_ptx ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 75
ptxas -arch=sm_75 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin
```

Rodinia compile-first smoke：
```bash
ARCH=75 bash tools/rodinia_ptx_smoke.sh
```

端到端（PoCL/driver，PTX 后端）快速运行（推荐一次性设置环境）：
```bash
source ventus-env/env.sh
export VENTUS_BACKEND=ptx

# 可选：不设置时默认 clamp 到 sm_75
export VENTUS_PTX_SM=75

# 可选：若不设置，driver 会默认用 $PWD/build/sbt_ptx 与 $PWD/ventus-env/spike/riscv/encoding.h
# export GPU_SBT_PTX="$PWD/build-gpu/sbt_ptx"
# export GPU_SBT_PTX_NO_COMMENTS=1
# export GPU_SBT_PTX_CACHE_DIR="/tmp/ventus_sbt_ptx_manual_$(date +%s)"
```

vecadd（最小端到端）：
```bash
cd ventus-env/pocl/examples/vecadd
../../build/examples/vecadd/vecadd 128 64
```

Rodinia（示例：bfs）：
```bash
cd ventus-env/rodinia/opencl/bfs
./run
```
