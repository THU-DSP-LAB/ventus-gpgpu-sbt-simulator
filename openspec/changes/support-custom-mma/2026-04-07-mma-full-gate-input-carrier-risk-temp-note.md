# 2026-04-07 临时问题记录：MMA full gate 输入载体风险（复验后升级）

- 状态：active temporary note
- 发现日期：2026-04-07
- 性质：验证方法风险记录（已完成去混叠复验；当前结论升级为“真实 lowering/contract 错配迹象”）
- 关联 change：`support-custom-mma`

## 背景

当前 `custom_mma_oracle.py --stage full` 已扩展覆盖首批非 `fp16->fp16` family（`f16->f32` / `bf16->f32` / `tf32->f32`，含 `m16n8*` 与 `m16n16* split-n`），但 full 对照出现“compile-first 全通过、semantic 全失败”的形态。

同时，`fp16->fp16` 路径仍按已知 checkpoint 维持显式 block/fail-fast，不纳入成功口径。

## 证据（首轮）

基于 2026-04-07 当天 gate 运行记录：

- `spike-precheck`：`spike_pass=6/7, spike_block=1, failures=0`
- `compile-first`：`compile_pass=6, compile_block=1, failures=0`
- `full`：`full_pass=0, full_block_skip=1, failures=6`

六个非 blocked family 在 full 均出现明显异常值（`nan` / `-inf` / 极大值）：

- `m16n8k16 f16->f32`
- `m16n8k16 bf16->f32`
- `m16n8k8 tf32->f32`
- `m16n16k16 f16->f32`
- `m16n16k16 bf16->f32`
- `m16n16k8 tf32->f32`

## 风险判定（首轮）

首轮判定时，microtest 的 `A/B` fragment 输入载体采用任意 `uint` bit-pattern 构造。该输入策略会把两类问题叠加：

1. `sbtsim` PTX tuple materialization / lowering 的真实语义错误；
2. Ventus LLVM frontend / Spike / builtin ABI 对 fragment bit-pattern 的 contract 不明确或未对齐。

因此首轮 full 失败不能直接作为“只属于 sbtsim lowering 局部 bug”的确定证据。

## 影响面

- 会直接影响 tasks `32/35` 的可判定性：semantic gate 结果目前不具备单一归因条件。
- 若不先修正输入载体，后续实现迭代可能在错误信号上反复调参，增加回归成本并掩盖真实跨组件 contract 问题。
- 该风险不等价于“所有 MMA 路径必须暂停”；它是验证方法层面的可信度风险。

## 去混叠复验（2026-04-07 后续执行）

在同日后续迭代中，已按本记录建议完成去混叠复验：将输入收紧为受控、有限、可解释的 `fp16/bf16/tf32` 编码集合后重跑 gate。

复验结果：

- `spike-precheck`: `spike_pass=6/7, spike_block=1, failures=0`
- `compile-first`: `compile_pass=6, compile_block=1, failures=0`
- `full`: `full_pass=0, full_block_skip=1, failures=6`

其中 `fp16->fp16` 仍是既有 block（工具链侧报错），不在本轮成功口径内。  
6 个非 blocked family 在受控输入后仍全部失败：

- `m16n8k16 f16->f32`
- `m16n8k16 bf16->f32`
- `m16n8k8 tf32->f32`
- `m16n16k16 f16->f32`
- `m16n16k16 bf16->f32`
- `m16n16k8 tf32->f32`

## 升级判定（基于复验）

去混叠后 full 仍全失败，说明问题已不再是“输入载体噪声导致的不可判定”，而是：

- 至少存在一个跨 family 的共享 lowering/fragment mapping 错配；或
- 仍存在未澄清的跨组件 contract 错配（frontend builtin ABI / Spike / sbtsim lowering）。

因此，问题性质已从“验证方法风险”升级为“真实 lowering/contract 错配迹象”。

## 局部推进方案（更新建议）

继续做局部可执行收敛，不改正式 contract：

1. 以“跨 6 个 family 的共享路径”为优先，排查共同层：lane mapping、A/B tuple materialization、C accumulator 取数、D writeback 与 split-n merge。
2. 先在 `m16n8 f16->f32` 上做最小可观测定位，再把修复推广到 `bf16/tf32/m16n16` 路径，避免并行盲改。
3. 每轮修复后重复 `spike-precheck -> compile-first -> full`，要求失败面单调收缩；若失败面不收缩，立即回滚该轮假设并换共享层假设。

## 是否需要升级成更大决策

当前结论：**暂不建议立即暂停开发**，先继续按共享 bug 线修复。

理由：

- 复验已完成去混叠，当前仍可先在 sbtsim 侧继续定位共享错配点。
- 若下一轮共享层修复后仍无法让失败面收缩，或出现与 `../llvm` / `../spike` 的明确 contract 冲突，再升级为需要与用户共同商议的跨组件决策问题。
