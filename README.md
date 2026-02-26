# 项目概述
本仓库实现 Ventus ELF 到 NVIDIA PTX 的原型级静态二进制翻译（SBT）流程，当前重点是：
- `ELF(.riscv) -> decode -> CFG verify -> PTX emit`
- compile-first（`ptxas` 可编译）与 Spike-vs-PTX 微测例对照
- 与 `ventus-env` 的 PoCL/driver 端到端联调

输入以 Ventus ELF 为准（当前阶段不使用 `.vmem`）：
- `ventus-env/rodinia/opencl/*/*.riscv`
- `ventus-env/pocl/build/examples/*/*.riscv`

## ISA 资料
- `VentusInst_basic.xlsx`：Ventus 指令表原始来源
- `VentusInst_basic.txt`：当前阶段提取后的文本版本（便于审阅与脚本处理）

## 仓库分层（当前）
- 核心实现：`sbt/`、`tools/`、`data/`、`testcases/ocl_compare/`
- 长期文档：`doc/`
- 规格/变更：`openspec/`
- 归档实验：`lab/`（已归档，不作为当前实现入口）
- 历史最小样例：`testcases/simple/`（已归档，不作为当前回归入口）

## 构建
```bash
cmake -S . -B build
cmake --build build -j
```

## 统一回归入口（推荐）
```bash
# 快速回归（不含端到端）：decode/emit + ptxas compile-first + PDS smoke + microtest gate
tools/regress.sh --preset quick --arch sm_75

# 全量回归（包含端到端）：在 quick 基础上增加 PoCL/driver 端到端回归
tools/regress.sh --preset all --arch sm_75

# 仅端到端（两种 runner 二选一）
tools/regress.sh --preset e2e --e2e-runner profile --timeout-scale 1.0
tools/regress.sh --preset e2e --e2e-runner ventus-env --jobs 8 --timeout-scale 1.0
```

## 阶段 1/2：解码与 CFG 验证
```bash
# 对照 .dump 校验 .text 字节
./build/sbt_decode verify ventus-env/rodinia/opencl/bfs/object0.riscv

# 列出函数符号
./build/sbt_decode funcs ventus-env/rodinia/opencl/bfs/object0.riscv

# pretty 输出（默认合并 regext 前缀）
./build/sbt_decode pretty ventus-env/rodinia/opencl/backprop/object0.riscv --func bpnn_layerforward_ocl

# 严格解码
./build/sbt_decode decode ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known >/dev/null

# CFG + setrpc/vbranch/join + barrier 校验（JSON）
./build/sbt_decode cfgverify ventus-env/rodinia/opencl/b+tree/object0.riscv --func findRangeK --require-known --json /tmp/findRangeK.cfg.json
```

## 阶段 3：Ventus -> PTX（compile-first）
```bash
# 生成 PTX（默认输出到 build/ptx/）
./build/sbt_ptx ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known

# 显式指定输出与目标 SM
./build/sbt_ptx ventus-env/rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 75

# ptxas 可编译性验证
ptxas -arch=sm_75 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# Rodinia compile-first smoke（11 kernels）
ARCH=sm_75 tools/rodinia_ptx_smoke.sh

# PDS 参数/映射 smoke
tools/pds_ptx_smoke.sh
```

## 阶段 4：PoCL/driver 端到端（SBT PTX JIT）
```bash
# 1) 构建本仓库工具
cmake -S . -B build
cmake --build build -j

# 2) 构建并安装 ventus-env driver
bash ventus-env/build-ventus.sh --build "driver"

# 3) 环境与后端选择
source ventus-env/env.sh
export VENTUS_BACKEND=ptx
export VENTUS_PTX_SM=75
export VENTUS_PTX_HEAP_MB=1024

# 4) PoCL 示例
cd ventus-env/pocl/build/examples/vecadd
./vecadd 128 64

# 5) Rodinia 示例
cd ventus-env/rodinia/opencl/bfs
./run
```

## 阶段 5：指令覆盖 gate + Spike-vs-PTX 微测例
```bash
source ventus-env/env.sh
cmake -S . -B build
cmake --build build -j

# 对照 + 全覆盖 gate（除 data/inst_exceptions.txt）
tools/microtest_coverage_gate.sh

# 浮点容差可调
tools/microtest_coverage_gate.sh --atol 1e-4 --rtol 1e-4

# want 列表更新
python3 tools/update_spike_want.py --dry-run
python3 tools/update_spike_want.py

# want 一致性 smoke
tools/check_spike_want_consistency.sh
```

## 回归耗时统计
```bash
source ventus-env/env.sh
export VENTUS_BACKEND=ptx
python3 tools/ventus_regression_profile.py --clean
```

## 文档与归档
- 实现文档索引：`doc/README.md`
- 工具清单与分层：`tools/README.md`
- 历史阶段快照：`doc/archive/`
- `lab/` 与 `testcases/simple/` 均为历史归档，不再作为当前实现与回归基线
