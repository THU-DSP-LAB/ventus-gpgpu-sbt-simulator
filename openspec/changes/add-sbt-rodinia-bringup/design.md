## Context
仓库已有分析与实验结论（不在本变更重复证明）：
- 解码与编码表来源：`lab/04_instruction_decode/README.md`
- 原型阶段总体方案与不支持范围：`lab/03_sbt_feasibility/summary.md`
- SIMT 结构化还原：`lab/03_sbt_feasibility/simtstack_hardware.md`
- warp-uniform 标量语义兜底：`lab/03_sbt_feasibility/scalar_uniform_ptx.md`
- 地址空间映射：`lab/03_sbt_feasibility/address_space.md`
- 函数调用可行性：`lab/03_sbt_feasibility/function_call.md` 与 `lab/03_sbt_feasibility/try/function_call/out/REPORT.md`
- 基于真实 `.dump` 的风险复核：`lab/03_sbt_feasibility/dump_risk_assessment.md`

本变更的目标是把上述内容“收敛”为一套分阶段交付与验收标准，作为后续实现的 gate。

## Facts (Rodinia inputs in this repo)
当前需要逐步正确处理的 ELF 输入（共 7 个）：
- `ventus-env/rodinia/opencl/b+tree/object0.riscv`（kernel：`findRangeK`）
- `ventus-env/rodinia/opencl/b+tree/object1.riscv`（kernel：`findK`）
- `ventus-env/rodinia/opencl/backprop/object0.riscv`（kernel：`bpnn_layerforward_ocl`、`bpnn_adjust_weights_ocl`）
- `ventus-env/rodinia/opencl/bfs/object0.riscv`（kernel：`BFS_1`、`BFS_2`）
- `ventus-env/rodinia/opencl/gaussian/object0.riscv`（kernel：`Fan1`、`Fan2`）
- `ventus-env/rodinia/opencl/kmeans/object0.riscv`（kernel：`kmeans_kernel_c`、`kmeans_swap`）
- `ventus-env/rodinia/opencl/nn/object0.riscv`（kernel：`NearestNeighbor`）

已观察到的关键输入特征（用于原型期约束/实现优先级）：
- ELF 为 `ELF32` + `.symtab` 存在，可用于函数边界与入口符号。
- `regext` 在 Rodinia 样本中出现，`regexti` 未观察到。
- `barrier` 在 `b+tree` 与 `backprop` 样本中出现。
- `_start` 中存在 `jalr t1` 启动跳转形态；kernel/函数体内的 `jalr` 主要为 `ret` 形态（见 call 分析报告）。

### Observed metrics (from existing `*.dump` and analysis scripts)

按 `.dump` 统计（仅用于实现优先级与约束确认，不作为语义规格来源）：

| object | regext | regexti | barrier | setrpc | vbranch(vb*) | join | jalr |
|---|---:|---:|---:|---:|---:|---:|---:|
| `b+tree/object0.dump` | 4 | 0 | 3 | 21 | 21 | 13 | 1 |
| `b+tree/object1.dump` | 4 | 0 | 2 | 16 | 16 | 10 | 1 |
| `backprop/object0.dump` | 17 | 0 | 6 | 13 | 13 | 9 | 1 |
| `bfs/object0.dump` | 0 | 0 | 0 | 12 | 12 | 6 | 1 |
| `gaussian/object0.dump` | 4 | 0 | 0 | 8 | 8 | 5 | 1 |
| `kmeans/object0.dump` | 0 | 0 | 0 | 12 | 12 | 9 | 1 |
| `nn/object0.dump` | 6 | 0 | 0 | 6 | 6 | 4 | 1 |

CFG 结构化验证（来自 `lab/03_sbt_feasibility/try/cfg_rebuild/rebuild_cfg.py verify 'ventus-env/rodinia/opencl/*/*.riscv'`）：
- objects=7
- vbranch_ok=88/88
- unsupported_jalr=7（每个对象 1 处，均来自 `_start` 启动跳转形态）

## Key Decisions (prototype defaults)
1) Translation unit
- 默认不翻译 `_start`，以 `kernel_name` 对应的函数符号为入口做可达性扫描（避免把启动代码的间接跳转形态引入 kernel 翻译约束）。

2) SIMT control flow strategy
- 默认走“CFG 结构化还原 + PTX/NVIDIA 硬件分歧/收敛”，不实现 software SIMT stack 兜底（原型期不满足约束则直接拒绝输入）。

3) Scalar (warp-uniform) correctness
- 以正确性为先：在 PTX 侧显式维护 `WarpCtx`，并在发散路径用 active-lane leader 执行标量副作用，避免标量状态分裂。

4) Addressing / memory model
- 原型期默认采用地址空间方案 B：保留 Ventus 32-bit 数值地址，并按 `doc/ventus-isa/others.md` 的区间做 shared/global 分流。
- Host 侧需将 ELF `PT_LOAD` 段按 `p_vaddr` 装载到 global backing buffer，以支持 `0x8000_0000~` 的静态数据访问。

5) Fail-fast policy
- 对不可结构化 CFG、非标准 `jalr`（computed goto）、`regexti`、无法静态解析 join 等情况：明确报错并退出（而不是生成“可能错误”的 PTX）。

## Prototype contract (stage 0 output)

### What we translate by default
- **入口选择**：以 `kernel_name`（来自上层 runtime/driver）定位 ELF 的函数符号（`.symtab`，`STT_FUNC`），从该符号作为翻译入口。
- **不翻译 `_start`**：`_start` 视为 toolchain/启动代码，原型期不要求其可翻译/可运行；其职责（分发、初始化）由 host/driver 与 PTX entry prologue 取代。

这意味着：Rodinia ELF 即便在 `_start` 中包含 `jalr t1`（间接跳转），也不会阻塞 kernel 翻译与运行 bring-up。

### Accepted inputs (prototype stage)
- `ELF32` RISC-V，且存在 `.symtab`，kernel 入口函数为 `STT_FUNC` 且位于 `.text`。
- `setrpc` 形态需能静态解析 join PC（Rodinia 已观察到典型 `auipc` + `setrpc` 模式）。
- `vbranch` 必须满足结构化约束（post-dominator / 单入口 / 无侧出口）；否则直接拒绝（不做 software SIMT stack 兜底）。
- kernel/普通函数体内 `jalr` 仅允许 `ret` 形态（`jalr x0, ra, 0`）。
- `barrier` 必须可证明处于收敛点（否则拒绝）。

### Explicit non-support (prototype stage)
- `regexti`（出现即拒绝）。
- kernel 内 computed goto / jump table / 任意间接跳转（非 `ret` 形态的 `jalr`）。
- 不可结构化 CFG（irreducible 或违反 join 结构约束）。
- 完整 ELF 重定位、“指令混入数据”的通用识别。
- 原子/一致性/缓存控制、精确浮点/除法 corner case 位级一致性。

### Diagnostics (fail-fast)
- 任何拒绝均需报告：
  - `elf_path`
  - `function`
  - `pc`
  - `reason`（例如 `unsupported.regexti` / `unsupported.jalr` / `invalid.vbranch_structure` / `invalid.barrier_diverged`）

## Stage gates & artifacts (stage 0 output)

### Gate 1: decode correctness (stage 1)
- 产物：
  - `decoded.json`：包含 `.text` 的解码结果（至少含 `pc/op/rd/rs*/imm/len` 与必要 flags）
  - 可选 `pretty.dump`：以接近 `llvm-objdump` 的格式输出，便于对照 `*.dump`
- 验收：
  - 对 Rodinia 7 个 ELF 的所有目标 kernel：解码无崩溃、无未知指令（在“支持子集”范围内），`regext` 前缀生命周期正确。

### Gate 2: CFG structural verify (stage 2)
- 产物：
  - `cfg_verify.json`：函数级统计、每条 `vbranch` 的 join 解析与结构化检查结果
- 验收：
  - 对 Rodinia 7 个 ELF 的所有目标 kernel：`vbranch_ok` 全通过；否则必须清晰拒绝并给出诊断。

### Gate 3: PTX compile (stage 3)
- 产物：
  - 每个 kernel 一个 `.ptx`（可读、可复现）
  - `ptxas` 编译产物（可选：`.cubin`/SASS dump）
- 验收：
  - `ptxas` 可编译（以本机 CUDA 工具链为准），且不依赖未定义行为（例如在发散路径执行 `bar.sync`）。

### Gate 4: end-to-end run (stage 4, optional)
- 产物：
  - 端到端运行脚本/命令（PoCL/driver 或独立 runner）
- 验收：
  - 先 bring-up 无 `barrier` 的 kernel（如 `bfs/gaussian/nn/kmeans`），再扩展到含 `barrier` 的 `b+tree/backprop`。
