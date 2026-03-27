# sbtsim

## 项目概述
本仓库实现 Ventus ELF 到 NVIDIA PTX 的原型级静态二进制翻译（SBT）流程，当前主路径为：

- `ELF(.riscv) -> decode -> CFG verify -> PTX emit`
- 以 `ptxas` compile-first、Spike-vs-PTX 微测例对照、PoCL/driver 端到端集成为主要验证方式
- 默认作为 [ventus-env](https://github.com/THU-DSP-LAB/ventus-env) 的子项目使用，假定上级目录 `..` 即为 `ventus-env`

当前输入以 Ventus ELF 为准，不使用 `.vmem`：

- `../rodinia/opencl/*/*.riscv`
- `../pocl/build/examples/*/*.riscv`

当前能力边界与支持范围以 `VentusInst_basic.txt`、`openspec/specs/` 和当前实现代码为准。诸如标量浮点、replicated scalar-state、direct-call value ABI 等内部实现细节不在本文件展开，详见 [doc/README.md](doc/README.md)。

## 主要产物
- `build/sbt_decode`
  - 对 Ventus ELF 做解码、pretty print、函数枚举与 CFG verify。
- `build/sbt_ptx`
  - 将指定 Ventus ELF 函数翻译为 PTX，供 `ptxas` compile-first 检查或 `ventus-env` 端到端运行使用。

## 快速开始
```bash
# 1) 构建
cmake -S . -B build
cmake --build build -j

# 2) 查看/校验某个函数的解码与 CFG
./build/sbt_decode pretty ../rodinia/opencl/backprop/object0.riscv --func bpnn_layerforward_ocl
./build/sbt_decode cfgverify ../rodinia/opencl/b+tree/object0.riscv --func findRangeK --require-known

# 3) 生成 PTX 并做 compile-first 验证
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 75
ptxas -arch=sm_75 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# 4) 跑推荐的快速回归
tools/regress.sh --preset quick --arch sm_75
```

## ISA 资料
- `VentusInst_basic.xlsx`：Ventus 指令表原始来源
- `VentusInst_basic.txt`：当前阶段需要支持的指令文本版本，便于审阅、脚本处理与覆盖统计

## 仓库分层
- 核心实现：`sbt/`、`tools/`、`data/`、`testcases/ocl_compare/`
- 长期维护文档：`doc/`
- 规格与变更：`openspec/`（当前 contract、活跃 change、历史归档）
- 归档实验：`lab/`
- 历史最小样例：`testcases/simple/`

## 构建
```bash
cmake -S . -B build
cmake --build build -j
```

如需将 `sbt_ptx`/`sbt_decode` 安装到某个前缀（例如集成到 `ventus-env` 的 `../install`），可执行：

```bash
cmake --install build --prefix ../install
```

Spike `encoding.h` 会在构建期生成并固化解码所需的 pattern 子集，运行期不再读取 want/encoding 文件。默认使用 `../spike/riscv/encoding.h`；如目录布局不同，可显式指定：

```bash
cmake -S . -B build -DSBT_SPIKE_ENCODING_H=/abs/path/to/spike/riscv/encoding.h
cmake --build build -j
```

## `sbt_decode` 常用命令
```bash
# 对照 .dump 校验 .text 字节
./build/sbt_decode verify ../rodinia/opencl/bfs/object0.riscv

# 列出函数符号
./build/sbt_decode funcs ../rodinia/opencl/bfs/object0.riscv

# pretty 输出（默认合并 regext 前缀）
./build/sbt_decode pretty ../rodinia/opencl/backprop/object0.riscv --func bpnn_layerforward_ocl

# 严格解码
./build/sbt_decode decode ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known >/dev/null

# CFG + setrpc/vbranch/join + barrier 校验（JSON）
./build/sbt_decode cfgverify ../rodinia/opencl/b+tree/object0.riscv --func findRangeK --require-known --json /tmp/findRangeK.cfg.json
```

## `sbt_ptx` 常用命令
```bash
# 生成 PTX（默认输出到 build/ptx/）
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known

# 显式指定输出与目标 SM
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 75

# ptxas 可编译性验证
ptxas -arch=sm_75 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# 多函数 direct call 原型声明回归
./build/ptx_emit_call_prototype_test

# replicated scalar-state / value ABI / divergence 回归
./build/ptx_emit_leader_lane_abi_test
```

当前实现仍坚持 fail-fast：遇到 unknown/unsupported 指令、不可接受的 CFG 形态、或当前未支持的 `jalr` 用法时直接报错退出，而不是静默降级。

## 统一回归入口（推荐）
```bash
# 快速回归（不含端到端）：decode/emit + ptxas compile-first + PDS smoke + microtest gate
tools/regress.sh --preset quick --arch sm_75

# 全量回归（包含端到端）：在 quick 基础上增加 PoCL/driver 端到端回归
tools/regress.sh --preset all --arch sm_75

# 仅端到端（两种 runner 二选一）
tools/regress.sh --preset e2e --e2e-runner profile --timeout-scale 1.0
tools/regress.sh --preset e2e --e2e-runner ventus-env --jobs 8 --timeout-scale 1.0

# 覆盖默认工作目录策略
tools/regress.sh --preset quick --in-place
tools/regress.sh --preset quick --workdir /tmp/sbtsim-regress
tools/regress.sh --preset quick --keep-workdir
```

`tools/regress.sh` 默认切换到临时目录执行，并在退出后自动删除目录，避免在当前路径残留 `mt_*`、`object0.*` 等中间文件。端到端 preset 会显式把 `GPU_SBT_PTX` 绑定到当前工作树的 `build/sbt_ptx`，避免误用 `../install/bin/sbt_ptx` 的旧安装产物。

## PoCL/driver 端到端
```bash
# 1) 构建本仓库工具
cmake -S . -B build
cmake --build build -j

# 2) 构建并安装 ventus-env 的 ptx 后端（包含 driver/ptx_device + sbt_ptx）
bash ../build-ventus.sh --build "ptx"

# 3) 环境与后端选择
source ../env.sh
export VENTUS_BACKEND=ptx
export VENTUS_PTX_SM=75
export VENTUS_PTX_HEAP_MB=1024

# 4) PoCL 示例
cd ../pocl/build/examples/vecadd
./vecadd 128 64

# 5) Rodinia 示例
cd ../rodinia/opencl/bfs
./run
```

若要确保端到端运行验证的是当前工作树中的翻译器实现，应显式导出 `GPU_SBT_PTX=$PWD/build/sbt_ptx`；`tools/regress.sh` 与 `tools/ventus_regression_profile.py` 在检测到该二进制存在时会自动这样做。

## 覆盖与性能回归
```bash
source ../env.sh
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

# 端到端回归耗时统计
export VENTUS_BACKEND=ptx
python3 tools/ventus_regression_profile.py --clean
```

## 文档索引
- 用户入口与命令用法：[README.md](README.md)
- 实现文档索引：[doc/README.md](doc/README.md)
- 当前实现真相（as-built）：[doc/IMPLEMENTATION_CODEMAP.md](doc/IMPLEMENTATION_CODEMAP.md)
- OpenSpec 入口与状态分层：[openspec/README.md](openspec/README.md)
- 当前仍活跃的 PTX 专项问题：[doc/ADDRESS_SPACE_SPECIALIZATION.md](doc/ADDRESS_SPACE_SPECIALIZATION.md)
- 工具清单与分层：[tools/README.md](tools/README.md)
- 历史 PTX 设计/讨论参考：`doc/archive/`
- `lab/` 与 `testcases/simple/` 均为历史归档，不再作为当前实现与回归基线；`openspec/specs/simple-ptx-prototype/spec.md` 仅保留 legacy 背景用途
