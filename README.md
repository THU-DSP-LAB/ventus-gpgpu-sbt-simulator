# sbtsim

## 项目概述
本仓库实现 Ventus ELF 到 NVIDIA PTX 的原型级静态二进制翻译（SBT）流程，当前重点是：
- `ELF(.riscv) -> decode -> CFG verify -> PTX emit`
- 正确生成 PTX，正确在 nvidia gpu 硬件上运行，运行结果与 ventus spike 对照
- 与 `ventus-env` 的 PoCL/driver 端到端集成运行

当前 PTX lowering 主线已落地为：
- replicated active-lane scalar state
- direct-call `mutable_state/machine_ctx/runtime_env` 三层 value ABI
- `vbranch/join` 不再依赖 full-`x` broadcast / join shim 维持标量状态协议

注：本项目默认假定自身作为 [ventus-env](https://github.com/THU-DSP-LAB/ventus-env) 项目的子项目集成进 Ventus 软件栈，
假定 `..` 上级目录即为 ventus-env 项目路径

输入以 Ventus ELF 为准（不使用 `.vmem`）：
- `../rodinia/opencl/*/*.riscv`
- `../pocl/build/examples/*/*.riscv`

## ISA 资料
- `VentusInst_basic.xlsx`：Ventus 指令表原始来源
- `VentusInst_basic.txt`：上表提取的文本版本，只包含当前阶段需支持的指令（便于审阅与脚本处理）

## 标量浮点（RV32F, Zfinx 模型）支持（当前）
- 模型：标量浮点值以 **f32 raw bits 存在 X 寄存器**（无独立 F 寄存器文件）。
- 已纳入 `VentusInst_basic.txt` 并支持 decode + PTX lowering（mnemonic 规范化：`.` -> `_`）：
  - `flw/fsw`
  - `fadd_s/fsub_s/fmul_s/fdiv_s/fsqrt_s`
  - `fmadd_s/fmsub_s/fnmsub_s/fnmadd_s`
  - `fsgnj_s/fsgnjn_s/fsgnjx_s`
  - `fmin_s/fmax_s`
  - `feq_s/flt_s/fle_s`
  - `fclass_s`
  - `fcvt_w_s/fcvt_wu_s/fcvt_s_w/fcvt_s_wu`
  - `fmv_w_x/fmv_x_w`
- rounding mode：`rm=DYN` 当前按 RNE 处理（CSR.frm 未建模）；`rm=RMM/Reserved` 直接 fail-fast 报错。

## 仓库分层（当前）
- 核心实现：`sbt/`、`tools/`、`data/`、`testcases/ocl_compare/`
- 长期文档：`doc/`
- 规格/变更：`openspec/`（当前 contract、活跃 change、历史归档）
- 归档实验：`lab/`（已归档，不作为当前实现入口）
- 历史最小样例：`testcases/simple/`（已归档，不作为当前回归入口）

## 构建
```bash
cmake -S . -B build
cmake --build build -j
```

如需将 `sbt_ptx`/`sbt_decode` 安装到某个前缀（例如集成到 `ventus-env` 的 `../install`），可执行：
```bash
cmake --install build --prefix ../install
```

Spike `encoding.h` 用于在构建期生成并固化解码所需的 pattern 子集（运行期不再读取 want/encoding 文件）。默认假定 `ventus-env` 位于上级目录 `..`，即使用 `../spike/riscv/encoding.h`；如目录布局不同，可显式指定：
```bash
cmake -S . -B build -DSBT_SPIKE_ENCODING_H=/abs/path/to/spike/riscv/encoding.h
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

# 覆盖默认工作目录策略
# 1) 在当前目录执行（旧行为）
tools/regress.sh --preset quick --in-place
# 2) 指定工作目录（不会自动删除）
tools/regress.sh --preset quick --workdir /tmp/sbtsim-regress
# 3) 临时目录模式下保留目录（用于排障）
tools/regress.sh --preset quick --keep-workdir
```

`tools/regress.sh` 默认会切换到临时目录执行，并在退出后自动删除该目录，避免在当前路径残留 `mt_*`、`object0.*` 等中间文件。
端到端 preset 会显式把 `GPU_SBT_PTX` 绑定到当前工作树的 `build/sbt_ptx`，避免误用 `../install/bin/sbt_ptx` 的旧安装产物。

## 阶段 1/2：解码与 CFG 验证
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

## 阶段 3：Ventus -> PTX（compile-first）
```bash
# 生成 PTX（默认输出到 build/ptx/）
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known

# 显式指定输出与目标 SM
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 75

# ptxas 可编译性验证
ptxas -arch=sm_75 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# 说明：当前 PTX lowering 主线采用 replicated active-lane scalar state：
# - live `x-reg` / scalar CSR 状态在当前 active lanes 上保持复制且相等；
# - 标量条件分支继续使用 `bra.uni`，但直接读取 replicated `%x`，不再做 leader broadcast；
# - 普通 scalar load 走 all-lane，scalar store 按需从当前 active mask 懒选择 leader；
# - `vmv.x.s` 保留固定 lane 0 语义，并把结果显式复制回 replicated `%x`；
# - `vbranch/join` 不再把整份 `%x` 当作控制流 payload，也不再插入 full-`x` join shim。

# 多函数 direct call 原型声明回归（前向调用）
./build/ptx_emit_call_prototype_test

# replicated scalar-state / value ABI / divergence 回归（target 名称暂沿用历史命名）
./build/ptx_emit_leader_lane_abi_test

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

说明：若要让端到端运行验证当前工作树中的翻译器实现，应显式导出 `GPU_SBT_PTX=$PWD/build/sbt_ptx`；`tools/regress.sh` 与 `tools/ventus_regression_profile.py` 在检测到该二进制存在时会自动这样做。

## 阶段 5：指令覆盖 gate + Spike-vs-PTX 微测例
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
```

## 回归耗时统计
```bash
source ../env.sh
export VENTUS_BACKEND=ptx
python3 tools/ventus_regression_profile.py --clean
```
该脚本会优先使用当前仓库的 `build/sbt_ptx`；如需覆盖，可自行设置 `GPU_SBT_PTX=/path/to/sbt_ptx`。

## 文档与归档
- 用户入口与命令用法：`README.md`
- 实现文档索引：`doc/README.md`
- 当前实现真相（as-built）：`doc/IMPLEMENTATION_CODEMAP.md`
- OpenSpec 入口与状态分层：`openspec/README.md`
- 当前仍活跃的 PTX 文档仅保留问题 2：`doc/ADDRESS_SPACE_SPECIALIZATION.md`
- 当前 contract specs：`openspec/specs/replicated-scalar-state/spec.md`、`openspec/specs/ptx-call-prototype/spec.md`、`openspec/specs/inst-support/spec.md`、`openspec/specs/sbt-rodinia-bringup/spec.md`、`openspec/specs/build-time-spike-pattern-subset/spec.md`
- Ventus LLVM 对 `vbranch` / `join` 下 SGPR/VGPR 有效性的源码分析参考：`doc/ventus-divergence-sgpr-analysis.md`
- PTX call 边界冷状态是否会被 `ptxas` 消去的实验背景：`lab/06_ptx_call_boundary_dead_state/README.md`
- 历史 PTX 设计/讨论参考：`doc/archive/PTX_LOWERING_MAIN_PROPOSAL.md`、`doc/archive/PTX_LOWERING_REDUCTION_PLAN.md`、`doc/archive/PTX_XREG_QUALITY_REGRESSION_REPORT_2026-03-15.md`、`doc/archive/PTX_LEADER_CTX_REUSE_DESIGN.md`、`doc/archive/PTX_DIRECT_CALL_VALUE_BLOB_ABI_DESIGN.md`、`doc/archive/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`
- 工具清单与分层：`tools/README.md`
- 历史阶段快照：`doc/archive/`
- `lab/` 与 `testcases/simple/` 均为历史归档，不再作为当前实现与回归基线；`openspec/specs/simple-ptx-prototype/spec.md` 仅保留 legacy 背景用途
