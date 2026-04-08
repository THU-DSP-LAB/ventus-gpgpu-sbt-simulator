# 代码实现梳理（Code Map）

> 状态：`current canonical implementation map`
>
> 本文是当前代码实现真相（as-built）的长期入口。
> 当 README、设计说明、实验记录与代码行为出现冲突时，应以本文和源码为准。

本文以“**源码真实入口**”描述当前仓库的实现结构与关键调用链，作为后续维护/排错的长期文档（尽量不过期）。若需要“带日期的实测结果与阶段结论”，请优先看归档快照 `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`（以及其引用的历史快照）。

## 0. 目标与边界（当前实现）

- 目标：把 Ventus 工具链产生的 RISC‑V `ELF32`（主要是 `ventus-env/rodinia/opencl/*/*.riscv`）按函数符号切片，做 SBT：`ELF → decode → CFG build → CFG verify → emit PTX`。
- 原型策略：fail-fast（遇到 unknown/unsupported/CFG verify 不通过直接退出），不做 software SIMT stack 兜底。
- 端到端运行（阶段 4）发生在 `ventus-env/driver/driver/ptx_device`，本仓库提供 `sbt_ptx` 作为“翻译器可执行文件”被 driver 调用（参见 `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`）。
- 当前 PTX driver contract 采用一个逻辑 `Global` 数值窗口；driver 侧以 CUDA VMM 预留整段 Global VA，并按需映射 ELF/PT_LOAD、runtime allocation、metadata 与 PDS 相关页。
- VMM 映射粒度仍受 CUDA allocation granularity 约束，但 runtime bump allocator 只按请求大小（16B 对齐）推进 `next_vaddr`；若 heap-window ELF `PT_LOAD` 与 runtime allocation 落在同一页，driver 以页级共享 claim 维持兼容，而不是把该页判成冲突。
- `next_vaddr` 只反映配置的 runtime heap window 使用情况；位于该 window 之外的 Global ELF segment 不会污染 runtime allocation cursor。PDS bitmap 扩容时若旧 bitmap 不是栈顶分配，driver 会直接切换到新的内部 bitmap，而不是把这类时序视为错误。
- 当前页回收策略偏向减少 launch 间抖动：当某页引用计数降到 0 时，driver 只把它标记为空闲缓存页，不会立刻 `cuMemUnmap/cuMemRelease`；这些空页仅在 `vt_dev_close()` 时统一释放，地址有效性仍通过活跃引用而不是“是否还留在页缓存表”判断。

## 1. 仓库目录与模块职责

### 1.1 `sbt/`：核心库（`sbt_lib`）

构建入口：`CMakeLists.txt` 中 `add_library(sbt_lib ...)`。

- `sbt/elf_reader.{hpp,cpp}`
  - 读 ELF section：`read_section(path, ".text")`，返回 `{vaddr,data}`。
  - 读函数符号：`read_func_symbols()`，当前只读 `.symtab`，筛选 `STT_FUNC` 且非 `SHN_UNDEF`。
  - 约束：不做 relocation；要求 `.symtab` 存在（目前是硬要求）。

- `sbt/spike_encoding_parser.{hpp,cpp}`
  - 解析 `ventus-env/spike/riscv/encoding.h` 中 `#define MATCH_*/MASK_*` 与 `DECLARE_INSN(...)`，得到 `name/match/mask`。
  - 说明：是纯文本解析器，不跑预处理器；主要供 build-time subset 生成器（`gen_spike_encoding_subset`）与少量工具/单测使用。

- `sbt/want_file.{hpp,cpp}`
  - Spike pattern 白名单（`DECLARE_INSN` id）的**单一输入源**：默认使用仓库内 `data/spike_want.txt`。
  - `load_spike_want_list(path)`：按行加载、去重、支持 `#` 注释。
  - 注意：不再提供 `GPU_SBT_WANT_FILE`/repo-root/CWD 的“自动路径解析”；want 文件路径由构建系统/脚本显式传入（如 CMake 调用 `gen_spike_encoding_subset --want-file ...`）。
  - 补充：`sbt_decode/sbt_ptx` 的 pattern 子集已在构建期固化，不再在运行期读取 want 文件；本模块主要供生成器与维护脚本使用。

- `sbt/riscv_decode.{hpp,cpp}`
  - `decode_text(text, vaddr, opt, patterns)`：按 4B 指令解码。
  - 支持 `regext/regexti` 前缀 bundling：前缀只作用下一条指令；CFG 里会把“bundle pc”和“真实指令 pc”区分开。
  - current：默认对连续前缀 fail-fast，报 `nested regext prefix`；若环境变量 `SBT_COMPAT_SPIKE_NESTED_REGEXT=1` 打开，则临时按 Spike 现有行为顺序覆盖前缀状态，允许同一条真实指令前出现连续 `regext/regexti`。
  - Ventus 扩展优先：先走 repository-local custom non-MMA decode（对齐上游 LLVM/Spike 的 `0x42/0x2A/0x5A/0x7A` opcode 口径），再按 `match/mask` 命中 Spike pattern，最后才走 RV32 标量子集解码。
  - MMA 边界（current）：`CustomFamily::Mma` 仅保留 ownership 位；`opcode=0x0A` 的 MMA decode/lowering 尚未在当前实现落地，`--require-known` 下保持 fail-fast。
  - `DecodedInst` 是当前“最小 IR”：含 `name`、寄存器类（X/V）、寄存器号、立即数类型与值、是否携带 regext 前缀信息，以及标量 FP rounding mode（`fp_rm`，来自 F 指令的 `rm` 域）。

- `sbt/cfg.{hpp,cpp}`
  - `build_function_cfg(decoded, func_start, func_end_excl)`：构建函数级 CFG。
  - `BundleInst`：`pc` 表示 bundle start（若有 regext 则为前缀 pc），`inst_pc` 是真实指令 pc；`len` 为 `4 + prefix_bytes`，常见为 4 或 8，开启 `SBT_COMPAT_SPIKE_NESTED_REGEXT=1` 后连续前缀场景可能为 12 及以上。
  - 基本块 leader 规则：函数入口、分支/跳转目标、terminator 后一条、以及 `join` 处会作为 leader。
  - 控制流分类：
    - `jal x0, off`：uncond jump（terminator）
    - `jal rd!=0, off`：call（非 terminator，fallthrough）
    - `jalr`：仅 `ret` 形态被识别为 return；其它 `jalr` 视为“间接控制流 terminator”（后续 verify 会记录为 unsupported）
    - `beq/bne/...` 与 `vb*`：条件分支（terminator，含 fallthrough）

- `sbt/cfg_verify.{hpp,cpp}`
  - `verify_function(cfg, func_name)`：对 `setrpc/vbranch/join/barrier/jalr` 做结构化验证。
  - `setrpc` join PC 解析：当前实现从 `setrpc` 向前回看近处 `auipc` 写同一寄存器（窗口大小固定，偏 bring-up）。
  - `vbranch` 校验核心：
    - 解析 join PC（来自最近一次 `setrpc`）
    - join 必须落在 `join` 指令处，且成为基本块入口
    - post-dominator / region side-exit / region single-entry 等结构化条件（循环形态有特殊放宽）
    - 额外：做一份“向量寄存器 uniform must 分析”，用于判断某些 `vbranch` 是否可证明 warp-uniform（从而对 barrier 合法性做更合理的保守处理）
  - `barrier` 校验：保守策略——`barrier` 所在块不得落在任何“不可证明收敛”的 vbranch 区域内。
  - 输出：`FunctionVerifyResult`（含每条 vbranch 与 barrier 的细节记录，以及 `unsupported_jalr` 列表）。

- `sbt/ptx_emit.{hpp,cpp}`
  - `emit_module(entry_cfg, sym_by_addr, entry_name, funcs, ptx_name_by_addr, opt)`：输出一个 PTX module，包含 1 个 `.entry <kernel>` + 若干 `.func <callee>`（用于 direct call）。
  - `emit_kernel(...)`：兼容接口（单函数 `.entry`，不含通用 call graph）。
  - 关键语义约定（当前主线）：
    - `setrpc/join/vsetvli`：结构化翻译下视为 no-op（主要用于 Stage2 verify）。
    - `barrier`：翻译为 `bar.sync 0;`（依赖 Stage2 barrier 合法性检查）。
    - 标量（x-reg）live state：采用 replicated active-lane 表示，任何仍然 live 的 `x-reg` / scalar CSR 在当前 active lanes 上都应保持相等。
    - leader 只在真正需要 single-lane 语义时按需选择：当前主线把 scalar store 等 externally side-effecting 指令降到 leader-only；普通 scalar ALU / branch / CSR read / load 直接 all-lane 执行。
    - 标量条件分支（`beq/bne/blt/bge/bltu/bgeu`）：保持 `bra.uni`，但直接读取 replicated `%x` 比较，不再做 leader-to-all-lane broadcast。
    - fixed-lane-sensitive：`vmv.x.s` 保留 architectural lane 0 语义；若 lane 0 不在当前 active mask 中则显式 `trap`，否则把 lane 0 结果 `shfl.sync` 复制回目标 `%x`。
    - all-lane scalar consumer：直接从 replicated `%x` 取值，覆盖 `vmv_v_x/vmv_s_x/vfmv_v_f`、`vmerge_vxm/vfmerge_vfm`、`vadd_vx` 及同类 `vx` 路径。
    - structured divergence：`vbranch/join` 不再把整份 `x-reg` 文件作为控制流 payload；路径入口与 join 前驱不再插入 full-`x` shim，只有真正的 leader-only scalar side effect 才会在 use point 懒选择 leader。
    - 标量浮点（RV32F, Zfinx 模型）：f32 以 raw bits 存在 X 寄存器；支持 `flw/fsw`、`fadd_s` 等标量 F 指令子集；`rm=DYN` 按 RNE 处理（CSR.frm 未建模），`rm=RMM/Reserved` fail-fast。
    - 数值地址空间：按区间把 u32 地址映射到 `.shared` 或 `.global`（`Shared + Global` current contract），对应 `Options::{shared_base_vaddr,global_base_vaddr}`；低于 shared window 的地址显式 `trap`。
    - `vlw.v/vsw.v`：按 Ventus PDS（private memory）语义实现为“全局 PDS buffer + 数值地址映射”：
      - `.entry` 参数包含 `pds_base_vaddr/pds_size_per_thread/pds_bitmap_base_vaddr/pds_pool_num_blocks`；
      - prologue 以 block 级原子方式从 bitmap 申请 PDS block，写入 shared；
      - `CSR_PDS = wg_pds_base + warp_id_in_block * (32 * pds_size_per_thread)`；
      - 再通过统一的数值地址映射 helper 落到 `.global` 访问。
  - 调用（call）：
    - 一小部分 builtin 仍在 emitter 内按名字内联（OpenCL id/query + 少量 helper）。
    - 其它 direct call（`jal ra, imm`）会翻译为 PTX `call.uni`，并要求被调函数也被翻译为 `.func`（由 `tools/sbt_ptx.cpp` 的 call graph 闭包收集保证）。
    - helper ABI 已切换到 `mutable_state_blob in/out + machine_ctx_blob in + runtime_env_blob in` 三层 value ABI；`vctx` 不再是主线参数。
    - mutable call state 当前显式携带 `leader_lane`、完整 logical `x-reg`、完整 logical `v-reg`；helper 入口会恢复 runtime/machine/mutable blobs，但 call marshal 不再为了 `x-reg` payload 额外做 leader broadcast。
    - `emit_module` 会先在模块头为所有 helper `.func` 发射 prototype，再发射函数体，避免前向调用触发 `requires call prototype` / `Unknown symbol`。
    - 非 `ret` 形态 `jalr` 仍属于 unsupported（原型期 fail-fast）。

### 1.2 `tools/`：CLI 与脚本（bring-up/回归）

- `tools/sbt_decode.cpp` → `build/sbt_decode`
  - `funcs`：列 `.symtab` 函数符号
  - `verify`：对照 `.dump` 校验 `.text` 字节一致（golden）
  - `pretty`：近似 objdump 输出
  - `cfgverify`：批量跑 Stage2 verify 并输出 JSON（含统计）
  - 注意：`sbt_decode/sbt_ptx` 的 Ventus pattern 子集在构建期由 `gen_spike_encoding_subset` 生成 `<build>/generated/spike_encoding_subset.hpp` 并编译进二进制；运行期不再读取 want/encoding 文件。

- `tools/sbt_ptx.cpp` → `build/sbt_ptx`
  - 主流水线：`read .text + .symtab → 入口函数切片 → decode/CFG/verify → 扫描 direct call → 收集可达函数闭包 → emit PTX module（.entry + .func）→ 写文件`。
  - PTX 输出：默认 `build/ptx/<bench>.<stem>.<func>.ptx`（路径规则偏 Rodinia 目录布局）。
  - 缓存：`<out>.meta` 记录输入 ELF/自身 exe 的时间戳与参数（pattern 子集已固化在二进制中），命中则直接复用已有 PTX。
  - 环境变量（行为开关/调试）：见 `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md` 与 `tools/sbt_ptx.cpp`。
  - 回归测试：
    - `build/ptx_emit_call_prototype_test`：覆盖“helper 前向调用 + prototype 先声明 + 新 value ABI prototype/definition 同步”。
    - `build/ptx_emit_leader_lane_abi_test`：覆盖 replicated scalar-state、fixed-lane `vmv.x.s`、direct-call value ABI、lazy leader selection 与 `vbranch/join` 无 full-`x` shim 的主线合同（target 名称沿用历史命名）。

- `tools/rodinia_ptx_smoke.sh`
  - 固定列表：Rodinia 11 个 kernel（compile-first），生成 PTX 并用 `ptxas` 编译。
  - 说明：当前是“最小 smoke”，偏 bring-up 期硬编码。

- `tools/regress.sh`
  - 统一回归入口：按 preset 聚合调用 compile-first/PDS/want/microtest gate/端到端回归等脚本与可执行文件。
  - 默认切换到临时工作目录执行并在退出时清理，避免污染调用目录；可通过 `--in-place/--workdir/--keep-workdir` 覆盖。
  - 端到端阶段会显式导出 `GPU_SBT_PTX=<repo>/build/sbt_ptx`，确保回归验证的是当前工作树刚构建出的翻译器，而不是 `install/bin` 中可能滞后的安装副本。

- `tools/ventus_regression_profile.py`
  - 调 `make` + 跑 ventus-env 下的 PoCL/Rodinia/testcases（端到端），统计每个 testcase 的 compile/run/total wall time，并可收集 `sbt_ptx` profile jsonl。
  - 说明：依赖 `ventus-env` 目录存在，且测试列表是硬编码 `TestCase` 数组。
  - 若调用者未显式设置 `GPU_SBT_PTX`，脚本会在检测到 `<repo>/build/sbt_ptx` 时自动绑定到该二进制，并打印所选路径。

- `tools/update_spike_want.py`
  - 从 `VentusInst_basic.txt`（Custom/V 部分）+ Spike `encoding.h` 更新 `data/spike_want.txt`（用于 pattern 输入）。

- `tools/check_spike_want_consistency.sh`
  - 最小 smoke：用当前 want+encoding 重新生成 subset header，并与构建期生成的 `<build>/generated/spike_encoding_subset.hpp` 做 diff；再确保 `sbt_decode/sbt_ptx` 在 `--require-known` 下能 decode/emit（用于防止 want 漂移/构建产物过期）。

- `tools/ventus_ocl_run.cpp` → `build/ventus_ocl_run`（可选构建）
  - OpenCL host runner：按 A/B buffer 约定跑指定 kernel，并把 B 写回/输出 hash。
  - 支持 `--in <raw-u32.bin>` 覆盖默认生成的 A buffer；用于 packed microtest 直接喂入原始 bit pattern，而不是在 kernel 内现场 pack。
  - 用于 Spike vs PTX 语义对照的 micro-test 执行器。

- `tools/ventus_ocl_compare.py` + `tools/microtest_coverage_gate.sh`
  - 以同一份 OpenCL 源码为输入，分别在 `VENTUS_BACKEND=spike` 与 `VENTUS_BACKEND=ptx` 下运行 kernel 列表，对比输出 B：
    - 整数/位运算：byte-exact；
    - 浮点：atol/rtol 容差。
  - current：若调用者未显式设置 `GPU_SBT_PTX`，脚本会自动绑定当前仓库 `build/sbt_ptx`，避免 PTX 对照误落到 `../install/bin/sbt_ptx` 等旧安装产物。
  - `--coverage`：对 `_start` + 各 kernel 导出 `sbt_decode --json`，调用 `tools/ventus_inst_coverage.py` 计算 `VentusInst_basic.txt` mnemonic 覆盖率，并按 `data/inst_exceptions.txt` 扣除例外。
  - 路径解析采用脚本绝对路径（`env.sh` 与 `ventus_inst_coverage.py`），不依赖调用时当前目录。

- `tools/custom_non_mma_oracle.py`
  - custom non-MMA 专用 gate：对 `testcases/ocl_compare/custom_non_mma_kernels.cl` 里的 microtests 逐个执行：
    - `VENTUS_BACKEND=spike` 与 `VENTUS_BACKEND=ptx` 结果对照；
    - 同一 ELF 的 `sbt_decode --require-known`；
    - 同一 ELF 的 `sbt_ptx --require-known` + `ptxas` compile-first。
  - current：这条 Spike-vs-PTX OpenCL buffer compare 路径就是当前 non-MMA custom families 的 canonical semantic oracle。
  - 比较规则：
    - shuffle / vcvt / packed arithmetic：精确比较；
    - `fp32` SFU：f32 容差比较；
    - packed `f16x2` / `bf16x2` SFU：按半精度 lane 容差比较。
  - current：shuffle 类 microtest 会在 oracle 中强制以完整 32-lane warp 规模执行，避免 `n < 32` 时因 runner 把 local size 降成 1 而引入伪失败。
  - current：若当前 custom kernel 编译产物出现连续 `regext/regexti` 指向同一条真实指令，oracle gate 通过显式 `--spike-compat-nested-regext` 仅对该验证链打开 `SBT_COMPAT_SPIKE_NESTED_REGEXT=1`；`sbt_decode/sbt_ptx` 默认行为仍保持 fail-fast。
  - 当前 packed SFU microtests 使用 raw packed 输入文件驱动；这是刻意将 “packed 输入构造” 与 “SFU 指令语义” 解耦，避免被 `vcvt_*_fp32 + pack_*x2` 的独立 contract 问题污染结论。

- `tools/custom_decode_test.cpp`
  - current non-MMA decode gate，覆盖 `shuffle/vcvt/packed/SFU` 的 repository-local decode 元数据与污染防护。
  - current：测试中仍显式断言 MMA placeholder opcode 在 `--require-known` 下必须失败，用于保证 non-MMA 与 MMA active change 的 ownership 边界不被静默打破。

- `tools/regext_bundle_test.cpp` → `build/regext_bundle_test`
  - `regext/regexti` bundling 边界情况的最小回归。

- `tools/archive/`
  - 历史工具/补丁快照归档目录（不作为当前构建与回归入口）。

### 1.3 `testcases/`：小测例（语义对齐）

- `testcases/simple/`：早期最小样例（手写 PTX + 对照 Ventus 汇编/ELF/dump）。
  - 注意：`compile.sh` 中存在用户路径/绝对路径；以及 `simple.S` 使用固定数值地址（0x9000_0000）这类早期假设，与当前端到端方案（driver+backing）并不完全一致；更适合作为“语义参考/历史产物”而非可复现脚本。

## 2. 关键调用链（从 CLI 到库）

### 2.1 `sbt_ptx` 的真实流水线

入口：`tools/sbt_ptx.cpp:main()`。

1. `sbt::elf::read_section(elf, ".text")`
2. `sbt::elf::read_func_symbols(elf)` → `sym_by_addr`（用于解析 call 目标符号 / 判断内联 builtin）
3. 以 `--func` 选择函数范围（依赖 `.symtab` 的 `addr/size`，size=0 时用“下一个符号”兜底）
4. 构建期生成并编译进二进制的 subset header（`<build>/generated/spike_encoding_subset.hpp`）→ `std::vector<sbt::Pattern>`
5. `sbt::decode_text(slice, func_start, DecodeOptions, patterns)`
6. `sbt::cfg::build_function_cfg(decoded, func_start, func_end)`
7. `sbt::cfg::verify_function(cfg, func)`（fail-fast）
8. 扫描 direct call，收集可达函数闭包（递归 decode/CFG/verify）
9. `sbt::ptx::emit_module(entry_cfg, sym_by_addr, func, funcs, ptx_name_by_addr, Options)` → 写 PTX 文件

### 2.2 `sbt_decode cfgverify` 的用途

入口：`tools/sbt_decode.cpp` 的 `cfgverify` 子命令。

- 用于批量扫 ELF 中的函数并输出 Stage2 verify JSON（便于快速发现：join 解析失败、结构化失败、barrier 不合法、出现非 ret 形态 `jalr` 等）。
- 当前实现按 `.symtab` 枚举函数符号，默认跳过 `_start`（可 `--include-start`）。

## 3. 当前实现的“硬前提/已知限制”（面向排错）

- **ELF 约束**：要求 `.symtab` 存在，且函数符号覆盖 kernel 入口；不做 relocation；对 strip/无符号的 ELF 不友好。
- **指令覆盖**：目标集合为 `VentusInst_basic.txt`（减去 `data/inst_exceptions.txt`）；在 `--require-known` 下遇到 unknown/unsupported 仍 fail-fast。
- **MMA 边界**：MMA 首批 matrix 与 lowering contract 目前属于 `active` 文档（`doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`、`doc/mma/LOWERING_ARCHITECTURE.md`），尚未成为 current as-built 实现。
- **控制流约束**：kernel 内 `jalr` 仅允许标准 `ret`；不可结构化 CFG 直接拒绝（不做 software SIMT stack）。
- **call 约束**：仅支持 direct call（`jal ra, imm`）+ 少量内联 builtin；非 `ret` 形态 `jalr` 仍 unsupported。
- **ABI/元数据**：当前 `.entry` 参数为 `(global_base, knl_vaddr, pds_base_vaddr, pds_size_per_thread, pds_bitmap_base_vaddr, pds_pool_num_blocks)`；helper `runtime_env_blob` 也只携带一个 `global_base`。prologue 仍会初始化 `x2/x8/x10`（其中 `x8(s0)` 先按 `_start` ABI 设置为 `CSR_LDS + CSR_NUMW*1024`，kernel 自身若有 `addi s0, s0, imm` 则视为 frame 分配，不在 prologue 中额外补偿）。
- **PDS（private）**：入口 prologue 由 `thread_linear_id==0` 原子申请/写回 `wg_pds_base`，kernel 退出前释放；`vlw.v/vsw.v` 与 `CSR_PDS` 都基于该 `wg_pds_base` 计算，不再按 full-grid block 线性编号寻址。
