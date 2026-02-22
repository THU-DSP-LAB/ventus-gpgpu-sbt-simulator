# 代码实现梳理（Code Map）

本文以“**源码真实入口**”描述当前仓库的实现结构与关键调用链，作为后续维护/排错的长期文档（尽量不过期）。若需要“带日期的实测结果与阶段结论”，请优先看归档快照 `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`（以及其引用的历史快照）。

## 0. 目标与边界（当前实现）

- 目标：把 Ventus 工具链产生的 RISC‑V `ELF32`（主要是 `ventus-env/rodinia/opencl/*/*.riscv`）按函数符号切片，做 SBT：`ELF → decode → CFG build → CFG verify → emit PTX`。
- 原型策略：fail-fast（遇到 unknown/unsupported/CFG verify 不通过直接退出），不做 software SIMT stack 兜底。
- 端到端运行（阶段 4）发生在 `ventus-env/driver/driver/ptx_device`，本仓库提供 `sbt_ptx` 作为“翻译器可执行文件”被 driver 调用（参见 `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`）。

## 1. 仓库目录与模块职责

### 1.1 `sbt/`：核心库（`sbt_lib`）

构建入口：`CMakeLists.txt` 中 `add_library(sbt_lib ...)`。

- `sbt/elf_reader.{hpp,cpp}`
  - 读 ELF section：`read_section(path, ".text")`，返回 `{vaddr,data}`。
  - 读函数符号：`read_func_symbols()`，当前只读 `.symtab`，筛选 `STT_FUNC` 且非 `SHN_UNDEF`。
  - 约束：不做 relocation；要求 `.symtab` 存在（目前是硬要求）。

- `sbt/spike_encoding_parser.{hpp,cpp}`
  - 解析 `ventus-env/spike/riscv/encoding.h` 中 `#define MATCH_*/MASK_*` 与 `DECLARE_INSN(...)`，得到 `name/match/mask`。
  - 说明：是纯文本解析器，不跑预处理器；用于 bring-up 工具链联动。

- `sbt/riscv_decode.{hpp,cpp}`
  - `decode_text(text, vaddr, opt, patterns)`：按 4B 指令解码。
  - 支持 `regext/regexti` 前缀 bundling：前缀只作用下一条指令；CFG 里会把“bundle pc”和“真实指令 pc”区分开。
  - Ventus 扩展优先：先按 `match/mask` 命中 Spike pattern；否则走 RV32 标量子集解码。
  - `DecodedInst` 是当前“最小 IR”：含 `name`、寄存器类（X/V）、寄存器号、立即数类型与值、以及是否携带 regext 前缀信息。

- `sbt/cfg.{hpp,cpp}`
  - `build_function_cfg(decoded, func_start, func_end_excl)`：构建函数级 CFG。
  - `BundleInst`：`pc` 表示 bundle start（若有 regext 则为前缀 pc），`inst_pc` 是真实指令 pc；`len` 为 4 或 8。
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
  - `emit_kernel(cfg, sym_by_addr, kernel_name, opt)`：把函数 CFG 直接 lowering 成 PTX。
  - 关键语义约定（原型实现）：
    - `setrpc/join/vsetvli`：结构化翻译下视为 no-op（主要用于 Stage2 verify）。
    - `barrier`：翻译为 `bar.sync 0;`（依赖 Stage2 barrier 合法性检查）。
    - 标量（x-reg）状态：存放在 per-warp shared 的 `WarpCtx`（当前实现的具体布局见 `doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md`）。
    - 标量副作用执行策略：支持 leader-only 或 all-lanes（由 `Options::scalar_exec_leader_only` 与 `GPU_SBT_SCALAR_LEADER_ONLY` 控制）。
    - 数值地址空间：按区间把 u32 地址映射到 `.shared` 或 `.global`（shared / ELF backing / heap backing），对应 `Options::{shared_base_vaddr,elf_base_vaddr,heap_base_vaddr}`。
    - `vlw.v/vsw.v`：当前按“private indexed ops”临时用 PTX `.local` 数组模拟（原因与限制见 `doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md` 的 PDS 说明）。
  - builtin 调用：仅支持白名单（OpenCL id/query + 部分 math helper），并在 emitter 内内联实现；其它 call 直接报 `unsupported.call`。

### 1.2 `tools/`：CLI 与脚本（bring-up/回归）

- `tools/sbt_decode.cpp` → `build/sbt_decode`
  - `funcs`：列 `.symtab` 函数符号
  - `verify`：对照 `.dump` 校验 `.text` 字节一致（golden）
  - `pretty`：近似 objdump 输出
  - `cfgverify`：批量跑 Stage2 verify 并输出 JSON（含统计）
  - 注意：其 pattern 白名单与 `sbt_ptx` 存在重复，且实现里为解决 pattern 名字生命周期做了“泄漏静态 vector”的折中做法（CLI 生命周期内可接受，但不利于库化）。

- `tools/sbt_ptx.cpp` → `build/sbt_ptx`
  - 主流水线：`read .text + .symtab → 切片 func range → decode → CFG → verify → emit PTX → 写文件`。
  - PTX 输出：默认 `build/ptx/<bench>.<stem>.<func>.ptx`（路径规则偏 Rodinia 目录布局）。
  - 缓存：`<out>.meta` 记录输入 ELF/encoding.h/自身 exe 的时间戳与参数，命中则直接复用已有 PTX。
  - 环境变量（行为开关/调试）：见 `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md` 与 `tools/sbt_ptx.cpp`。

- `tools/rodinia_ptx_smoke.sh`
  - 固定列表：Rodinia 11 个 kernel（compile-first），生成 PTX 并用 `ptxas` 编译。
  - 说明：当前是“最小 smoke”，偏 bring-up 期硬编码。

- `tools/ventus_regression_profile.py`
  - 调 `make` + 跑 ventus-env 下的 PoCL/Rodinia/testcases（端到端），统计每个 testcase 的 compile/run/total wall time，并可收集 `sbt_ptx` profile jsonl。
  - 说明：依赖 `ventus-env` 目录存在，且测试列表是硬编码 `TestCase` 数组。

### 1.3 `testcases/`：小测例（语义对齐）

- `testcases/simple/`：早期最小样例（手写 PTX + 对照 Ventus 汇编/ELF/dump）。
  - 注意：`compile.sh` 中存在用户路径/绝对路径；以及 `simple.S` 使用固定数值地址（0x9000_0000）这类早期假设，与当前端到端方案（driver+backing）并不完全一致；更适合作为“语义参考/历史产物”而非可复现脚本。

## 2. 关键调用链（从 CLI 到库）

### 2.1 `sbt_ptx` 的真实流水线

入口：`tools/sbt_ptx.cpp:main()`。

1. `sbt::elf::read_section(elf, ".text")`
2. `sbt::elf::read_func_symbols(elf)` → `sym_by_addr`（用于 call builtin 解析）
3. 以 `--func` 选择函数范围（依赖 `.symtab` 的 `addr/size`，size=0 时用“下一个符号”兜底）
4. `sbt::spike::parse_declared_insns(encoding.h)` + bring-up whitelist → `std::vector<sbt::Pattern>`
5. `sbt::decode_text(slice, func_start, DecodeOptions, patterns)`
6. `sbt::cfg::build_function_cfg(decoded, func_start, func_end)`
7. `sbt::cfg::verify_function(cfg, func)`（fail-fast）
8. `sbt::ptx::emit_kernel(cfg, sym_by_addr, func, Options)` → 写 PTX 文件

### 2.2 `sbt_decode cfgverify` 的用途

入口：`tools/sbt_decode.cpp` 的 `cfgverify` 子命令。

- 用于批量扫 ELF 中的函数并输出 Stage2 verify JSON（便于快速发现：join 解析失败、结构化失败、barrier 不合法、出现非 ret 形态 `jalr` 等）。
- 当前实现按 `.symtab` 枚举函数符号，默认跳过 `_start`（可 `--include-start`）。

## 3. 当前实现的“硬前提/已知限制”（面向排错）

- **ELF 约束**：要求 `.symtab` 存在，且函数符号覆盖 kernel 入口；不做 relocation；对 strip/无符号的 ELF 不友好。
- **指令覆盖**：bring-up whitelist + RV32 子集；未覆盖指令直接 unknown/unsupported。
- **控制流约束**：kernel 内 `jalr` 仅允许标准 `ret`；不可结构化 CFG 直接拒绝（不做 software SIMT stack）。
- **builtin 白名单**：call 仅支持 emitter 内硬编码集合；超出则 `unsupported.call`。
- **ABI/元数据**：当前 PTX kernel 参数固定为 `(elf_base, heap_base, knl_vaddr)`，并在 prologue 初始化 `x2/x8/x10`（其中 `x8(s0)` 先按 `_start` ABI 设置为 `CSR_LDS + CSR_NUMW*1024`，kernel 自身若有 `addi s0, s0, imm` 则视为 frame 分配，不在 prologue 中额外补偿）。
- **PDS（private）**：`vlw.v/vsw.v` 暂用 PTX local memory 模拟，属于 bring-up 期权宜实现，不等同于真实 driver/Spike 的 PDS 分配语义。
