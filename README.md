# 项目概述
乘影(Ventus)开源GPGPU项目是一个基于RISC-V及其V扩展指令集的学术界GPGPU项目    
包含指令集定义、软件栈与编译器实现、Chisel RTL与仿真器实现等    
编程模型（SIMT）与底层硬件结构与NVIDIA GPU有相似之处，本仓库希望尝试将乘影GPGPU的指令做静态二进制翻译为PTX指令，从而在NVIDIA GPU上做快速的功能仿真

# 乘影相关信息
乘影Ventus的指令集(ISA)存在两部分：标量部分与向量部分
* 标量：主要基于RV32IMA_zicsr_zfinx，这类似于NVIDIA SASS中的Uniform datapath(per-warp)
* 向量：主要基于RISC-V V扩展，但经过重解释以适配GPGPU的需求。类似于NVIDIA SASS中per-thread的普通指令
* 额外增加了一些标量/向量的Custom指令以适配GPGPU的需求

Ventus ISA向量部分对RVV指令语义的修改（待完善）
* 基本不使用RVV的访存指令
* 完全不支持所有RISC-V C扩展指令
* 不使用RVV的mask机制，v0寄存器是普通向量寄存器。使用自定义的vbranch系列（例如vbeq, vbne等）指令、setrpc指令、join指令实现基于SIMT stack的warp分支管理

# 项目结构
项目文件结构（目前项目刚刚开始，文件数量很少）
* README.nd, AGENTS.md
* `VEntusInst_basic.xlsx` 是Ventus ISA的基本指令表。第A列标注了指令的分类例如Custom/RV32I/M/V等；第B~AG列是指令的二进制格式；第AH列是指令的汇编格式；第AI列是指令的简单描述；第AJ列是备注信息；第AK列的yes/no标识本项目目前是否需要考虑此指令，标注no的行直接可忽略。
  * 文件中存在较多的合并单元格，读取时需要注意
  * Custom指令描述比较详细，RISC-V官方定义的指令如果没有重解释可能只在第AH列列出指令名
* 将 xlsx 中当前阶段需要关注的指令提取到纯文本文件 `VentusInst_basic.txt` 中便于阅读

# 项目进行
本项目使用 C++ 20 语言标准，推荐使用新语言特性
使用 CMake 做项目编译

阶段 1（ELF 读取 + 解码/对照）工具：
```bash
cmake -S . -B build
cmake --build build -j

# 校验 ELF 的 .text 字节与 *.dump 一致（golden：ventus-env 现有 dump）
./build/sbt_decode verify ventus-env/rodinia/opencl/bfs/object0.riscv

# 列出 ELF 的函数符号（用于找到 kernel 入口）
./build/sbt_decode funcs ventus-env/rodinia/opencl/bfs/object0.riscv

# 以函数符号为入口做 pretty 输出（默认会合并 regext 前缀）
./build/sbt_decode pretty ventus-env/rodinia/opencl/backprop/object0.riscv --func bpnn_layerforward_ocl

# 严格模式：遇到 unknown 指令直接失败
./build/sbt_decode decode ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known >/dev/null

# Stage 2：CFG + setrpc/vbranch/join 结构化验证 + barrier 合法性检查（输出 JSON）
./build/sbt_decode cfgverify ventus-env/rodinia/opencl/b+tree/object0.riscv --func findRangeK --require-known --verbose >/tmp/findRangeK.cfg.json
```

阶段 3（Ventus → PTX，compile-first）工具：
```bash
cmake -S . -B build
cmake --build build -j

# 生成某个 kernel 的 PTX（默认输出到 build/ptx/，避免写入 ventus-env/）
./build/sbt_ptx ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known

# 也可显式指定输出与目标 SM（默认 --sm 75）
./build/sbt_ptx ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 75

# 用 ptxas 做可编译性验证（先用 ptxas --list-arch 查看本机支持的架构；原型期建议用 sm_75）
ptxas -arch=sm_75 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# 一键 smoke：对 Rodinia 11 个 kernel 生成 PTX 并用 ptxas 编译
ARCH=sm_75 tools/rodinia_ptx_smoke.sh
```

阶段 4（PoCL/driver 端到端，SBT PTX JIT）：
```bash
# 1) 构建本仓库工具（需要 build/sbt_ptx）
cmake -S . -B build
cmake --build build -j

# 2) 构建并安装 ventus-env driver（产物在 ventus-env/install/lib/）
bash ventus-env/build-ventus.sh --build "driver"

# 3) 配置环境并选择后端
source ventus-env/env.sh
export VENTUS_BACKEND=ptx

# 可选：强制 PTX .target（原型期默认 clamp 到 sm_75，避免 sm_89 + PTX .version 7.0 的兼容性问题）
export VENTUS_PTX_SM=75

# 可选：heap 大小（MiB，默认 1024）
export VENTUS_PTX_HEAP_MB=1024

# 4) 跑 PoCL vecadd（会自动编译 object0.riscv，并触发 driver 侧 SBT 翻译+JIT）
cd ventus-env/pocl/build/examples/vecadd
./vecadd 128 64

# 5) 跑 Rodinia（示例：bfs）
cd ventus-env/rodinia/opencl/bfs
./run
```

回归耗时分析/加速（当前 PoCL Ventus 侧有一些较慢的 shell-out）：
```bash
# 输出每个 testcase 的 compile/run/total wall-time（结果写到 build/ventus-regression-profile/summary.json）
source ventus-env/env.sh
export VENTUS_BACKEND=ptx
python3 tools/ventus_regression_profile.py --clean

# 可选：绕过 pocl_ventus.cc 中对 assemble.sh 和 nm|grep 的调用（PTX 后端不依赖这些产物）
# 注：已经在PoCL中修复此问题，可以忽略
cmake -S . -B build
cmake --build build -j
export VENTUS_POCL_FASTPATH=1
export LD_PRELOAD=$PWD/build/libventus_pocl_fastpath.so
python3 tools/ventus_regression_profile.py --clean
```

设想feature：
* “用户态”仿真：不支持多虚拟地址空间
* device ABI兼容：不需要对现有软件栈做太多修改，直接兼容原有host-devie约定（指令语义 + kernel meta (CTA调度器接口+metadata buffer, 这些信息稍后大都保存在CSR中) + 内存视图）
  * 必要的kernel meta 采用 PTX kernel param 形式传递，ventus kernel args 被内嵌于 CSR_KNL 指向的地址中按照 `_start` 中的方案取出即可
  * 库函数采用同语义替换（`_start`, `get_global_id`等）

项目进度: 
* 原理验证阶段，在 `lab/` 目录下做一些 idea 有效性验证实验（大部分完成，后续编码过程中可能需要补充实验）
* 当前：最小原型阶段
