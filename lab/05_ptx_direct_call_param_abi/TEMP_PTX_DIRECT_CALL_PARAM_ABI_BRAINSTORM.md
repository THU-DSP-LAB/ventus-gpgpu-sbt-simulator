# PTX Direct Call 参数 ABI 方案讨论纪要（临时）

本文记录围绕 `doc/PTX_LOWERING_REDUCTION_PLAN.md` 中“问题 4”所做的一轮方案讨论。目标不是立刻定实现，而是先把需求背景、源码现状、候选方向、争议点与后续实验项整理清楚，供后续实测后再做决策。

临时性质说明：
- 本文是 brainstorm 结果沉淀，不是最终设计，不构成实现承诺。
- 当前优先级是“把问题定义清楚，并给出可验证的实验方向”，不是立即重构 direct call lowering。

## 1. 需求背景

当前仓库已经支持：
- Ventus ELF -> decode -> CFG verify -> PTX emit；
- kernel 内 direct call（`jal ra, imm`）闭包收集；
- PTX `.entry + .func + call.uni` 的基本工作流。

当前 direct call 的主要痛点不是“功能不通”，而是“PTX lowering 对调用的状态维护过于粗糙”，导致 PTX 指令条数和 `.local` 访存条数膨胀明显。

我们关心的问题是：
- 在保持当前 Ventus 语义边界的前提下，如何降低 direct call 的状态传递成本；
- 尤其是，是否应继续由本项目手工决定“哪些状态放寄存器、哪些状态放存储”，还是更多依赖 NVIDIA `ptxas`/JIT 后端自行做寄存器分配、spill 和调用优化。

## 2. 当前源码现状

当前实现采用的是“context ABI”而不是“value ABI”：
- entry 中分配 `.local .b8 __sbt_vctx[1024]`；
- caller 在普通 helper call 前，把 `%v0..%v255` 全量 `st.local.u32` 到 `vctx`；
- `call.uni` 时通过 `.param` 传递 `elf_base`、`heap_base`、`wctx_ptr`、`lds_ptr`、`knl_vaddr`、`pds_*`、`warp_id`、`warps_per_block`，以及可选的 `vctx_base`；
- callee 入口把 `%v0..%v255` 全量 `ld.local.u32` 从 `vctx` 读回；
- callee `ret` 前再全量 `st.local.u32` 写回 `vctx`；
- caller call 返回后再全量 `ld.local.u32` restore。

对应源码位置：
- `emit_vctx_store_all()` / `emit_vctx_load_all()`：`sbt/ptx_emit.cpp`
- `emit_direct_call()`：`sbt/ptx_emit.cpp`
- call lowering 与 return spill：`sbt/ptx_emit.cpp`
- direct call 闭包收集与递归拒绝：`tools/sbt_ptx.cpp`

从当前实现可直接推导出：
- 一次普通 helper call 的固定成本约为 1024 次 `.local` 访存级别的 `vctx` 搬运。
- `mod.need_vctx = !funcs.empty()` 是模块级粗开关；只要模块中存在任意非内联 helper，整套 `vctx` 模型就启用。
- builtin 虽然不走通用 `vctx` 路径，但当前仍统一先写 `ra` 并 `warp_sync`，成本模型尚未彻底分层。

## 3. 已讨论出的候选方向

### 3.1 方向 A：继续沿当前模型，做 selective spill / helper 摘要

思路：
- 保留当前 `vctx` 上下文模型；
- 通过 callsite liveness 或 helper 摘要，缩小 spill / restore 的寄存器集合；
- 逐步过渡到 caller-saved / callee-saved 风格的 Ventus 调用 ABI。

优点：
- 与当前实现连续，局部改动可控；
- 理论上能直接减少 `.local` 读写量。

缺点：
- 需要跨函数正确追踪 `uses/defs/clobbers`；
- 若摘要或传播分析不 sound，容易引入功能错误；
- 当前讨论中，对“先上摘要分析”存在明确顾虑。

当前结论：
- 该方向不是被否定，而是暂不作为本轮优先方案。

### 3.2 方向 B：改用 full-state `.param` value ABI，尽量把物理放置交给 `ptxas`

思路：
- 不再通过 `vctx` 共享整块向量上下文；
- 改为把 Ventus 需要跨调用保持的状态，显式编码成 PTX device function 的 `.param` 输入/输出；
- 让 NVIDIA 后端自行决定哪些值落入 SASS 物理寄存器，哪些值溢出到 local / stack，是否内联 call 等。
- 可以通过 ptx inline hint 来尽量激活 ptxas 的优化？

这个方向的出发点是：
- Ventus 需要模拟的不只有 256 个向量寄存器；
- 还包括标量寄存器、CSR、以及弥合两套 ISA 语义差异所需的额外 PTX 临时状态；
- 既然最终物理寄存器总量必然紧张，那么本项目自己手工决定“谁驻寄存器、谁进存储”，未必会比 `ptxas` 做得更好。

优点：
- 更符合“语义由前端表达，物理分配由后端决定”的职责分层；
- 有机会借助 `ptxas` 的寄存器分配、局部 spill、参数布局与内联优化；
- 避免过早在本项目中固化一套复杂且脆弱的手工寄存器缓存/摘要体系。

缺点：
- 是否真的优于当前 `vctx` 模型，必须依赖真实 `ptxas` / CUDA JIT 实测；
- `.param` ABI 只保证“合法表达调用接口”，不保证后端一定把巨大接口优化成高质量寄存器传参；
- 若接口过大，可能只是把问题从“显式 `vctx` 搬运”转成“超大 `.param` ABI 压力”。

当前结论：
- 这是当前讨论里更偏向继续探索的方向；
- 但必须先做实验，不能在没有实测前直接拍板替换现有模型。

### 3.3 方向 C：混合方案

思路：
- 保留一部分上下文模型；
- 同时对少量热点状态做显式 `.param` ABI；
- 其余仍走 context pointer 或聚合对象。

优点：
- 折中；
- 可逐步迁移。

缺点：
- 模型复杂度高；
- 在没有实验数据前，很容易同时背上两套机制的维护负担。

当前结论：
- 暂不优先展开。

## 4. 当前讨论中的关键争议点

### 4.1 是否应依赖 helper 摘要 / call graph 分析

支持担忧：
- 一个 Ventus helper 即使本体不使用某个寄存器，也可能调用其他 helper；
- 如果摘要只看本体，不追踪完整调用链，极易漏掉真实依赖；
- 这种错误属于静默语义错误，调试成本高。

补充事实：
- 当前实现已经构建 direct-call 闭包并拒绝递归/互递归，调用图在原型期是可枚举 DAG；
- 因此“做传递式摘要”在工程上并非做不到，但需要非常谨慎的 soundness 验证。

当前判断：
- 该争议尚未完全解决；
- 但本轮倾向是不把“先做摘要分析”作为最近一步。

### 4.2 `ptxas` 能否自动消除“大接口中的无用状态”

讨论中的两个观点：

观点 A：
- 如果某个 Ventus 寄存器最终映射到 PTX 虚拟寄存器，而 callee 并未真正使用它，那么 `ptxas` 可能自己看出它无用；
- 因此不必由前端先裁剪接口。

观点 B：
- PTX 虚拟寄存器是函数内作用域；
- caller 的 PTX `%v124` 与 callee 的 PTX `%v124` 不是“同一套跨函数共享的寄存器文件”；
- 因此跨调用的状态连续性仍然需要通过 ABI 或上下文对象显式表达；
- `ptxas` 也许能优化函数内无用寄存器，但未必会自动把巨大 call ABI 收缩到理想程度。

当前判断：
- 两个观点并不完全冲突；
- 更准确的表述是：`ptxas` 可以负责“物理放置与局部优化”，但能否显著收缩超大 `.param` 接口，需要实测验证，而不能仅靠推理假设。

### 4.3 是否要把所有 Ventus 状态都显式纳入 value ABI

已明确的现实约束：
- 不仅有 256 个向量寄存器；
- 还可能需要标量寄存器、CSR 和桥接状态；
- SASS per-thread 物理寄存器预算有限，不可能所有状态都长期留在物理寄存器中。

因此本轮讨论达成的一点共识是：
- “value ABI + 交给 `ptxas` 决定物理寄存器/栈放置”是一个可以严肃实验的方向；
- 但“它一定比我们自己做 selective spill 更优”目前还没有证据。

## 5. 当前暂定方案方向

当前更倾向继续探索的方向是：
- 不优先引入 helper 摘要裁剪；
- 优先评估“full-state 或 near-full-state `.param` value ABI”；
- 让 `ptxas` 负责物理寄存器分配、spill、参数布局和可能的内联；
- 最终依据真实 PTX -> SASS 结果判断该方向是否值得替代现有 `vctx` 模型。

这不是最终设计，而是“下一阶段实验假设”。

## 6. 后续实验建议

在真正决定是否转向 `.param` value ABI 之前，建议围绕以下问题做实验：

1. 与当前 `vctx` 模型相比，full-state `.param` ABI 生成的 PTX 是否明显更短。
2. `ptxas` 产出的 SASS 中，call 前后是否仍出现大量 local spill / stack traffic。
3. 对短 helper，`ptxas` 是否会自动内联或显著压缩 call 边界。
4. 大接口下寄存器数、spill 数和 occupancy 是否恶化。
5. 如果改用聚合输入/输出对象，而不是分散参数，`ptxas` 表现是否更差或更好。

建议实验输出至少包含：
- PTX 文本大小；
- `ptxas -v` 的寄存器数 / spill / stack / local 使用；
- 反汇编后的 SASS 里 call 前后搬运模式；
- 与当前 `vctx` 版本的功能一致性对比。

本仓库中的首轮实验骨架已落在：
- `lab/05_ptx_direct_call_param_abi/`
- 该目录当前先采用“手写/生成 PTX 微基准 -> `ptxas` -> `cuobjdump`”方式隔离 direct-call ABI 影响，暂不直接改主线 `sbt/ptx_emit.cpp`

## 7. 当前未决事项

以下问题暂未定论，需要实验后再决定：
- value ABI 采用“分散参数”还是“聚合参数对象”；
- 返回路径采用“单个聚合返回对象”还是 inout 风格；
- 标量寄存器、CSR 和桥接状态是否全部进入统一 ABI；
- builtin 路径是否继续特殊处理，还是也逐步并入统一调用模型；
- 是否保留 `vctx` 作为回退路径。

## 8. 本文用途

本文的用途是：
- 作为本轮 brainstorm 的临时纪要；
- 为后续实验设计提供一致的背景与问题表述；
- 避免在没有实验数据前过早把某个方案写成“已定方向”。

待后续实验完成后，应根据结果决定：
- 是把本方向升级为正式设计文档；
- 还是仅将本文归档，保留为一次被证伪或被部分证实的探索记录。
