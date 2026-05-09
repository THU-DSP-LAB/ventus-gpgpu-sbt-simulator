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
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 89
ptxas -arch=sm_89 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# 4) 跑推荐的快速回归
tools/regress.sh --preset quick --arch sm_89
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

当前 Spike-backed 非 custom 指令的 decode / CFG verify metadata 也已经收敛到仓库内显式维护的共享 contract。若要新增一条 Spike-backed 指令，当前同步入口至少包括：
- `data/spike_want.txt`
- `sbt/instruction_metadata.cpp`
- 重新构建生成的 build-time Spike subset（`cmake --build build` 会自动触发）

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
./build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx --sm 89

# ptxas 可编译性验证
ptxas -arch=sm_89 /tmp/BFS_1.ptx -o /tmp/BFS_1.cubin

# 多函数 direct call 原型声明回归
./build/ptx_emit_call_prototype_test

# replicated scalar-state / value ABI / divergence 回归
./build/ptx_emit_leader_lane_abi_test

# CFG verifier builtin call summary 回归
./build/cfg_verify_builtin_call_semantics_test

# external mnemonic contract / emitter allowlist 回归
./build/external_mnemonic_contract_test
python3 tools/check_ptx_emit_name_allowlist.py
```

当前实现仍坚持 fail-fast：遇到 unknown/unsupported 指令、不可接受的 CFG 形态、或当前未支持的 `jalr` 用法时直接报错退出，而不是静默降级。

当前 PTX emitter 的寄存器 current 口径是：
- 固定 machine/runtime/control 槽位继续保留，当前至少包括 `%r0/%r1/%r2`、`%p0`、`%rd0/%rd2/%rd4`、`%r26..%r29`，以及逻辑寄存器文件 `%x<256>` / `%v<256>`。
- lowering scratch 统一走函数级唯一命名的 `%tmp*` 虚拟临时寄存器，例如 `%tmp_b32_*`、`%tmp_b64_*`、`%tmp_p_*`、`%tmp_f32_*`、`%tmp_b16_*`、`%tmp_u8_*`、`%tmp_u16_*`；这些寄存器在函数头统一 `.reg` 声明，本阶段不要求重排固定槽位编号。

当前 instruction metadata / scalar execution current 口径是：
- Spike-backed 非 custom 指令通过共享 `InstId + InstMetadata` contract 提供 `operand_form`、`imm_kind`、`uniform_transfer_kind`；`DecodedInst.name` 只保留给 pretty print、JSON 输出与 external mnemonic contract。
- `sbt/riscv_decode` 在 Spike-backed pattern decode 与 scalar decode 两条路径上都会填充共享 metadata；custom non-MMA / MMA 继续保留各自显式 metadata，不回退到字符串推断。
- `sbt/cfg.cpp`、`sbt/cfg_verify.cpp` 与 `tools/sbt_ptx.cpp` 当前统一通过共享 `EmitDescriptor` / ordinary metadata 控制流 helper 消费 `branch/jump/call/return/indirect terminator/structured control/auipc` 语义；supported-path 控制流不再按 `DecodedInst.name` 做 correctness 分派。
- `sbt/cfg_verify` 的 vector uniform 传播消费共享 `uniform_transfer_kind`，并对 direct call 使用 ELF symbol map 做 call-aware transfer：已知 inlined builtin 应用共享 summary；ordinary/unresolved/缺 symbol map 的 direct call 按 Ventus ABI 清除 caller-saved `%v0..%v31` 的 vector-uniform facts、保留 callee-saved `%v32..%v255` facts；`sbt_ptx` 对 reachable direct-call callee 传播 call-site 入口 uniform/convergence facts，用于跨函数 barrier 收敛性验证。supported-path 指令缺 metadata 或缺控制流结构化语义时都会直接报错，不再回退到 `_vx/_vi/_vv/_v` suffix 或 mnemonic 猜测。
- PTX emitter 的 scalar execution classification 现为显式表驱动、默认拒绝未分类项；当前 supported scalar subset 必须逐条声明 `uniform-pure` / `lane-sensitive` / `fixed-lane-sensitive` / `externally-side-effecting`。

当前 lowering authority / mnemonic contract 口径是：
- 当前 PTX emitter 已收敛为 `sbt/ptx_emit.cpp`（public API、`EmitError`、module assembly）、`sbt/ptx_emit_internal.hpp`（internal shared interface / `EmitCtx` contract）以及按 ownership 拆分的实现单元：
  - core/function assembly：`sbt/ptx_emit_core.cpp`
  - runtime/PDS：`sbt/ptx_emit_runtime.cpp`
  - memory/address mapping：`sbt/ptx_emit_memory.cpp`
  - call ABI：`sbt/ptx_emit_call.cpp`
  - builtin lookup/summary：`sbt/builtin_semantics.cpp`
  - builtin PTX emission：`sbt/ptx_emit_builtin.cpp`
  - domain lowering：`sbt/ptx_emit_{control,scalar,vector,custom,mma_lowering}.cpp`
  - scalar FP：`sbt/ptx_emit_scalar_fp.cpp`
  current supported correctness path 继续消费 `DecodedInst.emit` / `DecodedInst.custom` / `DecodedInst.mma`，ordinary/custom/MMA 的 emit 语义不再由 `DecodedInst.name` 决定。
- `sbt/control_semantics.cpp`、`sbt/cfg.cpp`、`sbt/cfg_verify.cpp` 与 `tools/sbt_ptx.cpp` 的 current supported control-flow correctness path 都已改为消费 decode 产出的结构化语义；ordinary/custom/MMA 的 emit 语义同样不再由 `DecodedInst.name` 决定。
- `DecodedInst.name` 当前允许用途限定为 pretty / JSON / diagnostics / coverage / ABI-visible builtin symbol / comments。
- `tools/check_ptx_emit_name_allowlist.py` 会静态检查拆分后的完整 emitter 文件集中残余 `name` 读取是否只剩 allowlist 用途，并检查 builtin public allowlist、inline dispatch 与 verifier summary 共用共享 lookup；`build/instruction_metadata_contract_test`、`build/cfg_verify_builtin_call_semantics_test`、`build/pds_vector_memory_test` 等代表性 supported-path 回归会继续做 control/emitter poison-name、builtin call summary 与 PDS vector-memory lowering 检查，并覆盖 poisoned non-`ret` `jalr` 仍被识别为 `unsupported_jalr`。
- decode 期间的 shared-metadata lookup 当前仍保留内部 name-keyed 查表；但 CFG build / CFG verify / direct-call 闭包扫描已不再把 `DecodedInst.name` 当作控制流 semantic authority。
- historical 记录分别见 `openspec/changes/archive/2026-04-18-reduce-lowering-name-dependence/`、`openspec/changes/archive/2026-04-18-unify-cfg-control-semantics/` 与 `openspec/changes/archive/2026-04-25-modularize-ptx-emit-lowering/`。

当前 custom support surface 已覆盖 repository-local decode + PTX lowering + Spike-backed OpenCL buffer compare 的以下家族：
- non-MMA：`shuffle`、`vcvt`、packed `f16x2/bf16x2` 算术，以及 `fp32` / packed `f16x2` / packed `bf16x2` SFU。
- MMA（要求 `.version 7.8` / `sm_89`）：`row.col` 的 `m16n8k16 f16->f16`、`m16n8k16 f16->f32`、`m16n8k16 bf16->f32`、`m16n8k8 tf32->f32`、`m16n16k16 f16->f16`、`m16n16k16 f16->f32`、`m16n16k16 bf16->f32`、`m16n16k8 tf32->f32`。其中 `m16n16*` 通过 committed `split-n` composite lowering 落到两个 native `m16n8*` PTX MMA；current MMA tuple materialization/writeback 使用 scratchless `shfl.sync.idx.b32` path，不再占用 MMA 专用 `.shared` scratch staging。

当前 `fp16 -> fp16` MMA 已作为 landed current subset 的一部分接入 `sbt_ptx`：
- direct-native：`m16n8k16 row.col f16->f16`
- committed split-`n` composite：`m16n16k16 row.col f16->f16`

当前已支持的 8 条 MMA family 都接入了 `Spike vs sbtsim PTX vs CPU reference` 三方比较。比较规则为：`fp16` 输出 `NaN` 按分类相等、非 `NaN` half lane 默认要求 `<= 1 fp16 ULP`；`f32` 输出按 gate 文档中的 `atol/rtol` 容差比较。默认 MMA gate 会覆盖至少一组较小随机样本和一组较大随机样本。其余 non-`row.col` / deferred / research / 非 current `fp16 -> fp16` family 仍保持显式 fail-fast。

`regext/regexti` 默认仍按严格 bundling 处理；若需临时兼容 Spike 对连续前缀的现有行为，可设置环境变量 `SBT_COMPAT_SPIKE_NESTED_REGEXT=1`。打开后，`sbt_decode` 与 `sbt_ptx` 在遇到连续 `regext`/`regexti` 指向同一条真实指令时，不再报 `nested regext prefix`，而是按 Spike 现有顺序覆盖前缀状态继续解码；这是临时兼容方案，不改变默认 fail-fast 路径。

当前 `.entry` prologue 会按当前 Ventus `_start` ABI 对齐运行时初始状态：初始化 `x2/x3/x4/x8/x10`，其中 `x3(gp)` 来自 ELF `__global_pointer$`，`x2/x8` 使用 `KNL_LDS_STACK_SIZE_PER_WF`，`CSR_PRINT` 通过 `CSR_KNL + KNL_PRINT_ADDR` 建模。

## 统一回归入口（推荐）
```bash
# 快速回归（不含端到端）：decode/emit + MMA full gate + ptxas compile-first + PDS smoke + microtest gate
tools/regress.sh --preset quick --arch sm_89

# 全量回归（包含端到端）：在 quick 基础上增加 PoCL/driver 端到端回归
tools/regress.sh --preset all --arch sm_89

# 仅端到端（两种 runner 二选一）
tools/regress.sh --preset e2e --e2e-runner profile --timeout-scale 1.0
tools/regress.sh --preset e2e --e2e-runner ventus-env --jobs 8 --timeout-scale 1.0

# 覆盖默认工作目录策略
tools/regress.sh --preset quick --in-place
tools/regress.sh --preset quick --workdir /tmp/sbtsim-regress
tools/regress.sh --preset quick --keep-workdir
```

`tools/regress.sh` 默认切换到临时目录执行，并在退出后自动删除目录，避免在当前路径残留 `mt_*`、`object0.*` 等中间文件。默认 `quick/all` 路径会以 `--mma-stage=full` 执行 custom MMA gate，确保当前已支持 MMA family 的 compile-first 与三方语义对照默认纳入统一回归。`custom_mma_oracle.py` 与 `custom_non_mma_oracle.py` 会先通过 `tools/ventus_feature_probe.py` 检测所选 `--env-sh` 对应的 `VENTUS_INSTALL_PREFIX/bin/clang` 与同一 ventus root 下的 `spike` 是否支持 MMA / Shuffle / SFU / VCVT / packed custom 指令；缺失时按 feature 显式打印 `[FEATURE]` 与 `SKIP`，只跳过依赖缺失 feature 的 OpenCL 语义 gate，不影响 decode/emit、Rodinia compile-first、PDS 与普通 microtest gate。`all` 是 `quick + e2e`，其中 custom non-MMA oracle 在 feature 可用时会串行执行 36 个 OpenCL kernel 的 Spike/PTX/compile-first 链路，端到端阶段还会继续运行 PoCL/driver 回归；日常修改优先用 `quick` 或单项 gate 定位问题。端到端 preset 会显式把 `GPU_SBT_PTX` 绑定到当前工作树的 `build/sbt_ptx`，避免误用 `../install/bin/sbt_ptx` 的旧安装产物。回归脚本收到 `INT/TERM` 时会终止当前 step 的 process group，避免中断后遗留 `ventus_ocl_run` / compiler 子进程。

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
export VENTUS_PTX_SM=89
export VENTUS_PTX_HEAP_MB=1024

# 4) PoCL 示例
cd ../pocl/build/examples/vecadd
./vecadd 128 64

# 5) Rodinia 示例
cd ../rodinia/opencl/bfs
./run
```

当前 PTX backend 的 current contract 是：

- runtime-visible 普通地址空间只区分 `Shared` 与 `Global`
- driver 端要求 CUDA VMM 支持；无 VMM 时显式失败，不保留 legacy 双 backing fallback
- `VENTUS_PTX_HEAP_MB` 仍用于限制 driver 在 `0x9000_0000` 以上的 runtime allocation 窗口；它不再代表 PTX 侧独立 `Heap` 地址空间
- driver 在单一 `Global` VMM backing 下按页稀疏映射，但 heap-window 内的小 runtime allocation 仍按请求大小推进；落在 heap window 的 ELF `PT_LOAD` 也必须能与已存在的 runtime 页共存
- heap-window 之外的 `Global` ELF `PT_LOAD` 可以存在，但不会推进 runtime allocation 游标；PDS bitmap 需要扩容时，driver 允许分配新的内部 bitmap，而不是要求旧 bitmap 必须仍是最后一个 allocation
- 当前阶段空闲 VMM 页不会在 `free` 时立刻 `unmap/release`；driver 会保留这些空页到 device close，以减少连续 kernel launch 场景下的反复映射开销

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

# 需要给 packed microtest 注入原始输入时，可直接用 runner 的 --in
./build/ventus_ocl_run --src testcases/ocl_compare/custom_non_mma_kernels.cl \
  --kernel mt_custom_vrsqrt_f16x2 --n 16 --in /tmp/vrsqrt_f16x2.in.bin --out /tmp/vrsqrt_f16x2.out.bin

# shuffle 类 microtest 需要完整 warp 条件；custom_non_mma_oracle.py 会自动把它们提升到 32-lane 执行。
# 当前 custom kernels 若出现连续 regext/regexti 前缀，需显式打开 Spike-compatible nested-prefix 兼容模式。
python3 tools/custom_non_mma_oracle.py --n 8 --spike-compat-nested-regext

# 检查当前工具链/Spike 是否支持 custom 回归所需 feature；缺失原因会逐项列出。
python3 tools/ventus_feature_probe.py --summary

# fp16 MMA 的 Spike-vs-CPU-reference 独立语义测例：
# host 随机 seed -> kernel 内有限 fp16 值映射；用于与当前全 MMA gate 共享 CPU reference 口径
python3 tools/fp16_mma_spike_cpu_ref.py --seed 0x20260413 --ulp-tol 1

# 当前已支持 MMA family 的统一三方 gate：
# 默认跑较小/较大两档随机样本；对 supported family 做 Spike / sbtsim PTX / CPU reference 三方检查
python3 tools/custom_mma_oracle.py --stage full --sm 89

# ventus_ocl_compare.py 在未显式设置 GPU_SBT_PTX 时，会自动绑定当前树的 build/sbt_ptx，
# 避免误用 ../install/bin/sbt_ptx 的旧安装产物
python3 tools/ventus_ocl_compare.py --src testcases/ocl_compare/custom_non_mma_kernels.cl \
  --kernels mt_custom_shuffle_idx mt_custom_shuffle_up mt_custom_shuffle_down mt_custom_shuffle_bfly --n 32

# want 列表更新
python3 tools/update_spike_want.py --dry-run
python3 tools/update_spike_want.py

# want 一致性 smoke
tools/check_spike_want_consistency.sh

# instruction metadata / decode+verify 合同 smoke
./build/instruction_metadata_contract_test

# 端到端回归耗时统计
export VENTUS_BACKEND=ptx
python3 tools/ventus_regression_profile.py --clean
```

## 文档索引
- 用户入口与命令用法：[README.md](README.md)
- 实现文档索引：[doc/README.md](doc/README.md)
- 当前实现真相（as-built）：[doc/IMPLEMENTATION_CODEMAP.md](doc/IMPLEMENTATION_CODEMAP.md)
- OpenSpec 入口与状态分层：[openspec/README.md](openspec/README.md)
- 当前 Global 地址空间 contract：[openspec/specs/global-address-space/spec.md](openspec/specs/global-address-space/spec.md)
- 当前 PTX temp register allocation contract：[openspec/specs/ptx-temp-register-allocation/spec.md](openspec/specs/ptx-temp-register-allocation/spec.md)
- 当前仍活跃的 PTX 专项问题：[doc/ADDRESS_SPACE_SPECIALIZATION.md](doc/ADDRESS_SPACE_SPECIALIZATION.md)
- 工具清单与分层：[tools/README.md](tools/README.md)
- 历史 PTX 设计/讨论参考：`doc/archive/`
- `lab/` 与 `testcases/simple/` 均为历史归档，不再作为当前实现与回归基线；`openspec/specs/simple-ptx-prototype/spec.md` 仅保留 legacy 背景用途
