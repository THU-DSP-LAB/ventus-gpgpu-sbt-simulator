## Context

本设计描述的是 **target behavior**，不是当前已实现行为。

本 change 依赖的 canonical current docs/specs 有：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `openspec/project.md`
- `openspec/specs/inst-support/spec.md`

本 change 同时依赖以下 active / historical 设计输入，但它们不是 current contract 本身：

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`
- `doc/mma/LOWERING_ARCHITECTURE.md`
- `lab/07_fp16_mma_ptx_probe/README.md`
- `lab/07_fp16_mma_ptx_probe/EXPERIMENT_REPORT.md`
- archived `openspec/changes/archive/2026-04-13-support-custom-mma/*`

当前 change 的起点已经不同于 archived `support-custom-mma` 时期：当时 `fp16 -> fp16` 的结论是“必须显式 blocked”，因为仓库内没有足够证据证明 Ventus register window、PTX operand tuple 和 Spike/CPU reference 三者能稳定对齐。现在 `lab/07_fp16_mma_ptx_probe/` 已经给出新的设计证据：

- `m16n8k16 row.col f16->f16` 已有 hand-written PTX 端到端正确结果；
- `ptx` JIT、`ptxas -> cubin`、Spike、CPU reference 在同一随机输入下可完全一致；
- 已确认至少三条可行 PTX operand materialization 路径，其中两条不依赖 `ldmatrix`；
- 当前问题已经从“这条 native PTX 指令是否可行”收敛为“sbtsim 主线采用哪条 repository-local lowering contract”。

因此，本设计把这项工作视为“已确认基本可行，可以进入正式实现 change”，而不是继续停留在 blocked 背景备注。

## Goals / Non-Goals

**Goals:**

- 把 `m16n8k16 row.col f16->f16` 纳入 current supported direct-native MMA subset。
- 把 `m16n16k16 row.col f16->f16` 纳入 current supported committed `split-n` MMA subset。
- 冻结 `fp16 -> fp16` 在 sbtsim 主线中的显式 lowering contract，避免继续依赖未验证的 fail-path 代码或 shape-only metadata 假设。
- 把 `Spike / PTX / CPU reference` 三方比较纳入这两条 family 的 current semantic validation boundary。
- 保持其它 `fp16 -> fp16` / 非 `row.col` / deferred / research MMA family 的显式 fail-fast 边界不变。

**Non-Goals:**

- 不把所有 `fp16 -> fp16` MMA 组合一并放开。
- 不把 `lab/07_fp16_mma_ptx_probe/` 直接提升为 current contract 文档。
- 不要求首版实现必须采用 raw-window 直连寄存器供给路径。
- 不在本 change 中修改上级 `ventus-env` 仓库源码来“修出一条新 ABI”；本 change 只收敛 sbtsim 仓库内的 current contract 与实现。

## Decisions

### 1. 基本可行性已足够支撑正式实现 change

本 change 明确把“是否基本可行”的答案定为“是”。

理由不是“所有 family 都已被手写 PTX 逐条证明”，而是：

- `m16n8k16 row.col f16->f16` 这个 native building block 已被 hand-written PTX 在 `PTX / Spike / CPU reference` 三方上验证；
- `m16n16k16 row.col f16->f16` 在 active MMA architecture 中本来就是 committed `split-n` composite family；
- 只要 `m16n8k16` 的 native building block 和 `n=[0,7] / [8,15]` 的 Spike-equivalent split contract 都成立，就已经具备进入实现 change 的条件。

这意味着本 change 不再沿用 archived `support-custom-mma` 中“`fp16 -> fp16` 默认 blocked”的 historical 前提。该 historical note 继续保留背景价值，但 current contract 是否支持，应由本 change 决定。

**Alternatives considered:**

- 等 `m16n16k16` 也先做成单独 hand-written PTX 端到端实验后再建 change：Rejected，因为当前 change 的目标是进入正式实现，`m16n16k16` 的风险已经可以由 committed `split-n` contract 和后续实现任务承接，不必继续卡在 exploratory 阶段。

### 2. 主线实现不保留 materialized logical tile，中间层只保留为语义坐标 contract

`lab/07` 现在已经证明两类 no-`ldmatrix` 路径都可行：

- `logical tile -> direct-load offsets -> mma.sync`
- `Ventus raw window -> gather/repack -> mma.sync`

结合当前 sbtsim 已落地的其它 MMA 路径，本 change 选择的主线 contract 不是“先构造一个 materialized logical tile buffer”，而是：

- 语义层必须冻结为 `VGPR window -> logical coordinates -> PTX native fragment tuple`
- 代码实现直接从 source window 组装 PTX tuple，不保留显式 logical tile 中间结果
- direct-window gather/repack 公式必须能被解释为 Spike-equivalent logical-coordinate mapping
- `lab/07` 中的 direct-raw / direct-logical probe 作为这条 contract 的已验证证据

这样做的原因是：

- 当前已落地的 `f16->f32 / bf16->f32 / tf32` MMA emitter 本来就没有 materialized logical tile buffer，而是用 ABI descriptor + lane/coord 公式直接构造 tuple；
- `m16n16k16 split-n` 仍然需要共享同一套 `A/B/C/D` 逻辑坐标切片与 merge 规则；
- 真正需要保留的是“可审计的坐标语义”，不是一个额外的中间存储形态。

**Alternatives considered:**

- 强制保留一个 materialized logical tile buffer：Rejected，因为这与当前已落地 MMA emitter 的实现风格不一致，也会引入额外 scratch / 搬运复杂度而没有必要收益。
- 继续依赖“VGPR window 顺序恰好等于 PTX tuple 顺序”的旧假设：Rejected，因为这正是此前 fail-path 不可信的根源之一。

### 3. `m16n8k16` 与 `m16n16k16` 共享同一套 native ABI descriptor contract

本 change 不为 `m16n16k16 row.col f16->f16` 另起一套 native PTX contract。它必须复用：

- 同一个 native building block：`mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16`
- 同一个 `PtxMmaAbiKey / PtxMmaAbiDesc`
- 同一套 direct window-to-tuple helpers

两者的区别只在 lowering plan：

- `m16n8k16`：`DirectNativeM16N8`
- `m16n16k16`：`SplitNInto2xM16N8`，固定 `col_offset={0,8}`

这样可以避免 `fp16 -> fp16` 新支持 family 在主线里出现两套彼此独立的 operand contract。

**Alternatives considered:**

- 为 `m16n16k16` 单独定义另一套 PTX tuple ABI：Rejected，因为这会破坏当前 active architecture 中“split-n 复用 native building block”的分层约束。

### 4. 当前语义 gate 必须扩展为 Spike / PTX / CPU reference 三方比较

对这次 change 而言，compile-first 已经不再是核心风险，真正风险在于：

- Spike logical-coordinate 语义是否被 emitter 正确复现；
- `fp16` 输出比较是否采用了合理容差；
- existing fail-path 代码是否携带了未经验证的错误假设。

因此，本 change 冻结的验证口径是：

- Spike 继续作为 Ventus-side canonical semantics；
- CPU reference 作为 independent mathematical oracle；
- PTX 结果必须同时与前两者对齐；
- 对 `fp16` 输出，finite 值采用 `<= 1 fp16 ULP` 或等价 documented tolerance，`NaN` 只比较分类。

这条验证 contract 先前已经在 `lab/07` 和 Spike-vs-CPU-ref 独立测例中出现过，本 change 只是把它提升为当前支持 family 的正式 gate 要求。

**Alternatives considered:**

- 只做 Spike-vs-PTX，不再看 CPU reference：Rejected，因为用户已明确要求引入 CPU reference 和合理容差，而且 `fp16` family 的 bring-up 本来就需要一个独立于 PTX lowering 的数学 oracle。
- 只做 CPU reference-vs-PTX，不再看 Spike：Rejected，因为 current Ventus semantic contract 的 canonical source 仍然是 Spike。

### 5. `lab/07` 保持 active experiment 身份，不直接充当 current contract

`lab/07_fp16_mma_ptx_probe/` 的价值已经从“孤立探索”升级为“设计证据”，但它仍不是 current spec。

本 change 的文档分层必须保持：

- `openspec/specs/` 和仓库根文档描述 current contract；
- `doc/mma/LOWERING_ARCHITECTURE.md` 描述长期 active architecture；
- `lab/07` 继续保留为 active experiment / evidence；
- archived `support-custom-mma` 中的 blocked note 继续保留为 historical 背景。

**Alternatives considered:**

- 直接把 `lab/07` README/REPORT 当 current spec 入口：Rejected，因为这会混淆 `current` 和 `active experiment` 的状态边界。

## Risks / Trade-offs

- **`m16n16k16` 证据强度不对称风险**：目前 hand-written PTX 的直接端到端证据主要集中在 `m16n8k16` native building block，`m16n16k16` 还需要靠 split-n implementation gate 补完。这个风险是已知且可接受的，因为它正好落在本 change 的实现任务里。
- **旧 fail-path 代码可能本身就错**：当前 blocked 路径里的实现不应被默认信任。该风险的应对方式是“允许直接修改或替换”，而不是为旧代码再包一层 fallback。
- **坐标公式漂移风险**：既然不保留 materialized logical tile，中间语义将体现在 tuple gather/repack 公式里；这些公式必须继续由同一组 ABI descriptor、Spike 对照和 gate 共同钉住。
- **文档状态漂移风险**：仓库当前多处文档仍把 `fp16 -> fp16` 描述为 current blocked。若实现落地但文档未同步，current/active/historical 口径会再次失真。
