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

- `sbt/instruction_metadata.cpp`
  - current shared instruction metadata 事实源：为 Spike-backed 非 custom 指令维护 repository-managed `InstId + InstMetadata`。
  - current 最小 contract：至少覆盖 `operand_form`、`imm_kind`、`uniform_transfer_kind`，并为当前 supported scalar subset 维护显式 `ScalarExecKind`。
  - current：该层还集中维护 emitter-facing `EmitDescriptor`，作为 `sbt/control_semantics.cpp`、`sbt/cfg.cpp`、`sbt/cfg_verify.cpp`、`tools/sbt_ptx.cpp` 与 PTX emitter supported-path control/emit lowering 的 shared authority。
  - 边界：shared metadata 当前内部仍按 mnemonic name 查表并产出 descriptor；这是 current decode 实现细节，而不是 downstream CFG / verify / call-graph / emitter correctness path 的 authority 来源。
  - 新增 Spike-backed 指令的当前同步入口：`data/spike_want.txt` + 本文件；缺任一侧都会在 decode/verify/test 上显式失败。

- `sbt/riscv_decode.{hpp,cpp}`
  - `decode_text(text, vaddr, opt, patterns)`：按 4B 指令解码。
  - 支持 `regext/regexti` 前缀 bundling：前缀只作用下一条指令；CFG 里会把“bundle pc”和“真实指令 pc”区分开。
  - current：默认对连续前缀 fail-fast，报 `nested regext prefix`；若环境变量 `SBT_COMPAT_SPIKE_NESTED_REGEXT=1` 打开，则临时按 Spike 现有行为顺序覆盖前缀状态，允许同一条真实指令前出现连续 `regext/regexti`。
  - Ventus 扩展优先：先走 repository-local custom decode（含已落地的 non-MMA 与 MMA 路径），再按 `match/mask` 命中 Spike pattern，最后才走 RV32 标量子集解码。
  - current：Spike-backed pattern decode 与 scalar decode 都会填充共享 metadata；前者不再以 mnemonic suffix 作为 operand/immediate 主事实源。若命中了当前 build-time Spike subset 但缺少共享 metadata，会显式 fail-fast。
  - MMA 边界（current）：`opcode=0x0A` 现已落地首批 committed `row.col` MMA decode/lowering；`DecodedInst.custom.family = CustomFamily::Mma` 仅承担 family ownership，shape/layout/type/window 信息由独立 `MmaInstInfo` 承载。当前 landed subset 为 `m16n8k16 f16->f16`、`m16n8k16 f16->f32`、`m16n8k16 bf16->f32`、`m16n8k8 tf32->f32`、`m16n16k16 f16->f16`、`m16n16k16 f16->f32`、`m16n16k16 bf16->f32`、`m16n16k8 tf32->f32`；deferred/research MMA 组合与非 current `fp16 -> fp16` 组合在 `--require-known` 下显式 fail-fast。
  - `DecodedInst` 是当前“最小 IR”：含 `name`、`inst_id`、共享 metadata 映射出的 operand/transfer/classification/emit-descriptor 字段、寄存器类（X/V）、寄存器号、立即数类型与值、是否携带 regext 前缀信息，以及标量 FP rounding mode（`fp_rm`，来自 F 指令的 `rm` 域）。

- `sbt/control_semantics.{hpp,cpp}`
  - current shared control-flow consumer helper：统一从 `BundleInst.inst.emit` / ordinary metadata 读取 main-pipeline 控制流语义。
  - current contract：
    - 为 `cfg/cfg_verify/tools/sbt_ptx` 统一分类 `scalar branch / vector branch / direct jump / direct call / return / indirect terminator / setrpc / join / barrier / vsetvli / endprg`
    - direct branch/jump/call target 统一按 `inst_pc + imm` 计算，避免消费者重新抄写一份 bundle-pc 规则
    - `setrpc` join 解析统一通过 `ScalarIntKind::Auipc` 回溯，而不是 mnemonic 比较
    - supported-path 控制流 descriptor 缺失、类型不匹配或 contract 自相矛盾时显式 fail-fast，不回退到 `DecodedInst.name`

- `sbt/cfg.{hpp,cpp}`
  - `build_function_cfg(decoded, func_start, func_end_excl)`：构建函数级 CFG。
  - `BundleInst`：`pc` 表示 bundle start（若有 regext 则为前缀 pc），`inst_pc` 是真实指令 pc；`len` 为 `4 + prefix_bytes`，常见为 4 或 8，开启 `SBT_COMPAT_SPIKE_NESTED_REGEXT=1` 后连续前缀场景可能为 12 及以上。
  - 基本块 leader 规则：函数入口、分支/跳转目标、terminator 后一条、以及 `join` 处会作为 leader。
  - 控制流分类 current 全部通过 `sbt/control_semantics.cpp`：
    - `jal x0, off`：uncond jump（terminator）
    - `jal rd!=0, off`：call（非 terminator，fallthrough）
    - `jalr`：`ret` 形态识别为 return；其它 `jalr` 视为“间接控制流 terminator”（后续 verify 会记录为 unsupported）
    - `beq/bne/...` 与 `vb*`：条件分支（terminator，含 fallthrough）
    - `join`：继续强制成为 leader；`endprg`：no-succ terminator
  - current：regext-bundled control-flow instruction 继续区分 `bundle pc` 与 `inst_pc`；target/fallthrough 计算使用 `inst_pc`，block identity 使用 bundle `pc`。

- `sbt/cfg_verify.{hpp,cpp}`
  - `verify_function(cfg, func_name, VerifyOptions)`：对 `setrpc/vbranch/join/barrier/jalr` 做结构化验证；当前兼容 overload 会以空 options 调用，direct call 在无 symbol map 时按保守边界处理。
  - `setrpc` join PC 解析：当前实现通过 `sbt/control_semantics.cpp` 从 `setrpc` 向前回看近处 `ScalarIntKind::Auipc` 写同一寄存器（窗口大小固定，偏 bring-up）。
  - `vbranch` 校验核心：
    - 解析 join PC（来自最近一次 `setrpc`）
    - join 必须落在 `join` 指令处，且成为基本块入口
    - post-dominator / region side-exit / region single-entry 等结构化条件（循环形态有特殊放宽）
    - 额外：做一份“向量寄存器 uniform must 分析”，用于判断某些 `vbranch` 是否可证明 warp-uniform（从而对 barrier 合法性做更合理的保守处理）
  - current：结构化控制流分析与 unsupported `jalr` 识别都只消费共享 control helper；vector uniform 传播消费共享 `InstMetadata.uniform_transfer_kind`，并对 direct call 使用 `VerifyOptions::sym_by_addr` 做 call-aware transfer。resolved inlined builtin 会应用 `sbt/builtin_semantics.*` 中的 verifier-visible summary；resolved non-builtin、unresolved target 或缺 symbol map 的 direct call 会清空全部 vector-uniform facts；accepted builtin 若缺 summary 会按 metadata drift 显式失败。
  - current：`_Z12get_local_idj` / `_Z13get_global_idj` 与 fixed-dim workitem/global id builtin 写入 lane-varying `%v0`；`_Z12get_group_idj` / `_Z15get_global_sizej` 仅在 dim 输入 `%v0` pre-call 已 proven uniform 时保留 uniform proof；workgroup id builtin 写入 work-group-uniform `%v0`；pure math helper 按输入 uniformity 传播。
  - `barrier` 校验：保守策略——`barrier` 所在块不得落在任何“不可证明收敛”的 vbranch 区域内。
  - 输出：`FunctionVerifyResult`（含每条 vbranch 与 barrier 的细节记录，以及 `unsupported_jalr` 列表）。

- `sbt/ptx_emit.hpp` + `sbt/ptx_emit.cpp` + `sbt/ptx_emit_internal.hpp` + `sbt/ptx_emit_{core,runtime,memory,call,builtin,control,scalar,vector,custom,mma_lowering,scalar_fp}.cpp`
  - `emit_module(entry_cfg, sym_by_addr, entry_name, funcs, ptx_name_by_addr, opt)`：输出一个 PTX module，包含 1 个 `.entry <kernel>` + 若干 `.func <callee>`（用于 direct call）。
  - `emit_kernel(...)`：兼容接口（单函数 `.entry`，不含通用 call graph）。
  - current 结构分层：
    - `sbt/ptx_emit.cpp`：public API、`EmitError`、module/function assembly 入口。
    - `sbt/ptx_emit_internal.hpp`：internal shared interface，保留 `EmitCtx` 字段、固定寄存器/ABI 常量、small inline primitive 与 helper declarations，不再承载 runtime/PDS、memory、call ABI、builtin、scalar FP、MMA materialization 的大段实现。
    - `sbt/ptx_emit_core.cpp`：`EmitCtx` constructor、寄存器声明、function header/body assembly、dispatcher、CFG traversal、fallthrough emission；dispatcher precedence 固定为 `control -> scalar -> vector -> mma -> custom`。
    - `sbt/ptx_emit_runtime.cpp`：entry/helper prologue、PDS acquire/release、CSR/PDS runtime helper、kernel metadata scalar load。
    - `sbt/ptx_emit_memory.cpp`：numeric address mapping、typed load/store helper、leader-only scalar store wrapper。
    - `sbt/ptx_emit_call.cpp`：helper signature、call parameter layout、mutable/machine/runtime blob marshal、direct `.func` call emission。
    - `sbt/builtin_semantics.{hpp,cpp}`：OpenCL/helper builtin symbol lookup、`BuiltinKind`、public inlined-builtin classification、iterable accepted entries 与 verifier-visible vector-uniform summaries 的单一事实源。
    - `sbt/ptx_emit_builtin.cpp`：builtin inline dispatch 与 builtin emission bodies；`is_inlined_builtin_call_name()` 与 control lowering 委托共享 builtin semantics lookup。
    - `sbt/ptx_emit_scalar_fp.cpp`：FP rounding normalization、`fclass` 与 scalar FP lowering。
    - `sbt/ptx_emit_{control,scalar,vector,custom,mma_lowering}.cpp`：按 semantic domain 分离的 lowering translation units；MMA materialization/writeback 实现在 MMA ownership file 中继续复用 `sbt/ptx_mma.*` planner/ABI helper。
  - 关键语义约定（当前主线）：
    - current supported correctness path 已 descriptor-driven：ordinary/control/scalar/vector path 消费 `DecodedInst.emit`，custom non-MMA 消费 `DecodedInst.custom`，MMA 消费 `DecodedInst.mma`；`DecodedInst.name` 不再是 emitter semantic authority。
    - emitter 中残余 `name` 读取当前只允许出现在 comments / diagnostics / external reporting；该 allowlist 由 `tools/check_ptx_emit_name_allowlist.py` 对完整 post-split emitter 文件集与 `ptx_emit_internal.hpp` 静态检查，并额外验证 shared builtin lookup / public allowlist / control dispatch / verifier summary 同步。
    - `setrpc/join/vsetvli`：结构化翻译下视为 no-op（主要用于 Stage2 verify）。
    - `barrier`：翻译为 `bar.sync 0;`（依赖 Stage2 barrier 合法性检查）。
    - PTX 寄存器 ownership：当前固定 machine/runtime/control 槽位保持 stable，至少包括 `%r0/%r1/%r2`、`%p0`、`%rd0/%rd2/%rd4`、`%r26..%r29`、`%x<256>`、`%v<256>`；`%rd1/%rd3` 仍保留为稳定的 legacy reserved slot，不作为共享 scratch 池重新分配。
    - helper scratch：函数内临时值按类型分配到唯一命名 `%tmp*` virtual temp（`.b32/.b64/.pred/.f32/.b16/.u8/.u16`），并在函数头统一 `.reg` 声明；当前阶段不要求通过重排固定槽位编号来引入这套 scratch 策略。
    - 标量（x-reg）live state：采用 replicated active-lane 表示，任何仍然 live 的 `x-reg` / scalar CSR 在当前 active lanes 上都应保持相等。
    - scalar execution classification（current）：由共享 metadata 显式给出 `uniform-pure` / `lane-sensitive` / `fixed-lane-sensitive` / `externally-side-effecting`；当前 supported scalar subset 中未分类项在进入 lowering 前直接 fail-fast，不再默认视为 `UniformPure`。
    - MMA lowering（current）：首批 committed `row.col` MMA 子集继续消费 `DecodedInst.mma`、`AbiDesc` 与 `ScalarTupleValue`，但 tuple construction/writeback 已改为 scratchless shuffle path；A/B/C 通过固定候选 `%v(base + i)` 的 `shfl.sync.idx.b32` materialize，D 通过 destination-side gather/merge 写回，f32 D tuple 在 shuffle 前显式 bitcast 到 `.b32`，非 full-active-warp 的 MMA native sub-op 入口直接 `trap`。
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
    - 一小部分 builtin 仍在 emitter 内按 ABI-visible symbol 内联（OpenCL id/query + 少量 helper）；symbol identity 与 verifier summary 由 `sbt/builtin_semantics.*` 共享维护。
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
    - `build/instruction_metadata_contract_test`：覆盖 `data/spike_want.txt <-> instruction_metadata.cpp` 同步、Spike-backed decode metadata、CFG build / CFG verify / direct-call scan 的 shared control semantics authority、poison-name 不变性（包括 poisoned non-`ret` `jalr` 仍报告为 `unsupported_jalr`），以及 metadata / control descriptor 缺失时的显式失败。
    - `build/cfg_verify_builtin_call_semantics_test`：覆盖 verifier 对 inlined builtin helper call 的 vector-uniform summary、ordinary/no-symbol direct-call 保守边界，以及 shared builtin lookup / summary / public classifier 的 drift 防护。
    - `build/external_mnemonic_contract_test`：覆盖 pretty / JSON / diagnostics / coverage / external builtin symbol 等 external mnemonic contract，确保 authority 迁移后 `DecodedInst.name` 仍稳定服务外部口径。
    - `build/custom_ptx_emit_test` / `build/mma_ptx_emit_test`：覆盖 custom non-MMA 与 current MMA lowering 的 `%tmp*` 声明/使用、native/composite tuple emission，以及 `ptxas` compile-first 合法性。
    - `python3 tools/check_ptx_emit_name_allowlist.py`：静态检查完整 post-split emitter 文件集中的 `name` 读取只剩显式 allowlist 用途，并检查 shared builtin lookup / dispatch / verifier summary 单一事实源。

- `tools/rodinia_ptx_smoke.sh`
  - 固定列表：Rodinia 11 个 kernel（compile-first），生成 PTX 并用 `ptxas` 编译。
  - 说明：当前是“最小 smoke”，偏 bring-up 期硬编码。

- `tools/regress.sh`
  - 统一回归入口：按 preset 聚合调用 compile-first/PDS/want/microtest gate/端到端回归等脚本与可执行文件。
  - 默认切换到临时工作目录执行并在退出时清理，避免污染调用目录；可通过 `--in-place/--workdir/--keep-workdir` 覆盖。
  - 端到端阶段会显式导出 `GPU_SBT_PTX=<repo>/build/sbt_ptx`，确保回归验证的是当前工作树刚构建出的翻译器，而不是 `install/bin` 中可能滞后的安装副本。

- `tools/ventus_feature_probe.py`
  - current custom OpenCL 语义 gate 的能力探针：oracle 默认按所选 `--env-sh` 实际导出的 `VENTUS_INSTALL_PREFIX/bin/clang` 检查对应 Ventus builtin，同时检查同一 ventus root 下 `spike/riscv/encoding.h`、`spike/riscv/insns/` 与 MMA/SFU 所需 `dependencies/mma-sim` / `dependencies/unfu`。
  - 覆盖 feature：MMA、Shuffle、SFU、VCVT、packed `f16x2/bf16x2`。探针只给出 available/unavailable 与缺失证据，不生成 mock 成功路径。
  - `custom_mma_oracle.py` 与 `custom_non_mma_oracle.py` 默认消费该探针结果；缺失 feature 时显式打印 `[FEATURE]` 与 `SKIP`，可用 feature 继续真实执行。

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
  - current：默认按 `tools/ventus_feature_probe.py` 的检测结果只运行当前工具链/Spike 已支持的 feature；缺失 feature 逐 kernel 打印 `SKIP` 与原因。`--no-auto-skip-features` 可关闭该行为以暴露原始编译/执行错误。
  - 当前 packed SFU microtests 使用 raw packed 输入文件驱动；这是刻意将 “packed 输入构造” 与 “SFU 指令语义” 解耦，避免被 `vcvt_*_fp32 + pack_*x2` 的独立 contract 问题污染结论。

- `tools/custom_non_mma_specs.py`
  - current custom non-MMA oracle 的 kernel 清单与比较模式数据；用于把测试数据维护从 `custom_non_mma_oracle.py` 的执行编排中拆出。

- `tools/mma_decode_test.cpp`
  - current MMA decode gate，覆盖 `MmaInstInfo`、support-class 边界与首批 committed/deferred/research family 的 repository-local decode 元数据。

- `tools/custom_mma_oracle.py`
  - MMA 专用 gate：对 `testcases/ocl_compare/custom_mma_kernels.cl` 里的 microtests 逐个执行：
    - 先通过一次 Spike 运行 materialize 同次编译产物 `object0.riscv`；
    - 同一 ELF 的 `sbt_decode --require-known`；
    - 同一 ELF 的 `sbt_ptx --require-known` + `ptxas` compile-first；
    - 对当前 supported family 的同一 kernel 做 `Spike / sbtsim PTX / CPU reference` 三方语义对照。
  - current：这条 OpenCL buffer compare 路径就是当前 landed MMA subset 的 canonical semantic oracle。
  - current：helper carrier 采用 branch-free 的有限值查表，刻意避免把与 MMA 无关的 `switch -> vbranch/join` helper lowering 差异误报成 MMA 语义失败。
  - current：gate 会先按目标 feature macro materialize 单-kernel 源文件，避免 Ventus PoCL 在多-kernel OpenCL 源上把首个 kernel 错当成 `--init` 入口。
  - current：所有已支持 MMA family 都走 `Spike vs sbtsim PTX vs CPU reference` 三方 gate；默认覆盖一组较小随机样本和一组较大随机样本。其余 non-current MMA family 继续显式 blocked。
  - current：默认按 `tools/ventus_feature_probe.py` 的 MMA 检测结果决定是否运行 OpenCL 语义 gate；缺失 feature 时打印 `SKIP custom MMA oracle gate` 与缺失证据。`--no-auto-skip-features` 可关闭该行为以暴露原始编译/执行错误。

- `tools/mma_cpu_ref.py`
  - current MMA 共享 CPU reference helper，覆盖当前已支持的 8 条 MMA family。
  - 负责 host 侧随机 seed 生成、kernel 对应的 `A/B/C` 载荷重建、CPU 参考输出计算，以及 `fp16` / `f32` 容差比较统计。

- `tools/fp16_mma_spike_cpu_ref.py`
  - `fp16 -> fp16` 的 `m16n8k16 row.col` / `m16n16k16 row.col` Spike-vs-CPU-reference 独立测例；当前用于与 MMA PTX gate 共享 repository-managed CPU reference。
  - 输入策略：host 侧生成随机 `u32` seed；kernel 再把 seed 映射到有限 `fp16` 值集合，保持输入随机性同时避免 NaN/Inf payload 噪声。
  - 比较规则：`NaN` 按分类相等，非 `NaN` half lane 按 `fp16` ULP 容差比较，默认要求 `<= 1 ULP`。
  - current：这是 current 全 MMA CPU reference helper 在 `fp16 -> fp16` 两条 family 上的独立 Spike 交叉验证资产，不直接替代统一三方 semantic gate。

- `tools/custom_decode_test.cpp`
  - current non-MMA decode gate，覆盖 `shuffle/vcvt/packed/SFU` 的 repository-local decode 元数据与污染防护。
  - current：该测试继续只覆盖 non-MMA decode；MMA decode/semantic gate 已拆到 `tools/mma_decode_test.cpp` 与 `tools/custom_mma_oracle.py`，避免 non-MMA 与 MMA gate 混淆。

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
- **MMA 边界**：当前 as-built 已支持首批 committed `row.col` MMA 子集（见上文 decode/oracle 入口）；当前 MMA tuple materialization/writeback 是 scratchless shuffle path，不再使用 MMA 专用 `.shared` scratch staging，具体见 `doc/mma/SCRATCHLESS_SHUFFLE_LOWERING.md`。更宽的 MMA matrix、deferred/research families 与剩余架构讨论仍由 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`、`doc/mma/LOWERING_ARCHITECTURE.md` 作为 `active` 文档维护。
- **控制流约束**：kernel 内 `jalr` 仅允许标准 `ret`；不可结构化 CFG 直接拒绝（不做 software SIMT stack）。
- **call 约束**：仅支持 direct call（`jal ra, imm`）+ 少量内联 builtin；非 `ret` 形态 `jalr` 仍 unsupported。
- **ABI/元数据**：当前 `.entry` 参数为 `(global_base, knl_vaddr, pds_base_vaddr, pds_size_per_thread, pds_bitmap_base_vaddr, pds_pool_num_blocks)`；helper `runtime_env_blob` 也只携带一个 `global_base`。prologue 会按当前 `_start` ABI 初始化 `x2/x3/x4/x8/x10`：`x3(gp)` 来自 ELF `__global_pointer$`，`x2/x8` 使用 `CSR_KNL + KNL_LDS_STACK_SIZE_PER_WF`，`x10(a0)` 来自 `CSR_KNL + KNL_ARG_BASE`；`CSR_PRINT` 当前按 `CSR_KNL + KNL_PRINT_ADDR` 建模。kernel 自身若有 `addi s0, s0, imm` 则视为 frame 分配，不在 prologue 中额外补偿。
- **PDS（private）**：入口 prologue 由 `thread_linear_id==0` 原子申请/写回 `wg_pds_base`，kernel 退出前释放；`vlw.v/vsw.v` 与 `CSR_PDS` 都基于该 `wg_pds_base` 计算，不再按 full-grid block 线性编号寻址。
