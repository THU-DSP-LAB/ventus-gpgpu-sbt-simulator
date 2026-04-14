## Why

当前 `fp16 -> fp16` MMA 仍在 current contract 中保持显式 blocked：

- `openspec/specs/inst-support/spec.md` 仍要求该 family 在 PTX lowering / compile-first / semantic validation 前显式 fail-fast；
- `README.md`、`doc/IMPLEMENTATION_CODEMAP.md` 与已归档 `support-custom-mma` change 也都沿用这一口径；
- 当前 `sbt/ptx_emit.cpp` 仍以 `unsupported.mma.fp16_fp16_contract_pending` 直接阻断该路径。

这个 blocked 结论最初来自 archived `support-custom-mma` change 中的临时 mismatch 记录：当时仓库内只有
“frontend builtin tuple arity / Spike shape window / decode metadata”之间存在未澄清差异的证据，还没有
能把 PTX operand 供给方式、Ventus raw window 形态和 Spike/CPU reference 三方同时钉住的实验结果。

`lab/07_fp16_mma_ptx_probe/` 的新实验已经改变了这个前提：

- `m16n8k16 row.col f16->f16` 的手写 PTX probe 已在当前 `sm_89` / CUDA 13.1 环境下通过；
- 同一输入生成与 CPU reference 口径下，`ptx` JIT、`ptxas -> cubin`、Spike、CPU ref 四者可以完全一致；
- 实验已经钉住了至少三条可行 PTX operand 供给路径：
  - `logical tile -> physical transpose -> ldmatrix -> mma.sync`
  - `logical tile -> direct-lane load offsets -> mma.sync`
  - `Ventus raw window -> A/C gather + B repack -> mma.sync`

因此，当前 `fp16 -> fp16` 的主要问题已经不再是“PTX/ABI 是否可行”，而是“仓库实现应采用哪条 repository-local lowering contract，并把它落实到 sbtsim 主线中”。这说明该需求已经具备从探究阶段转入正式实现 change 的条件。

同时，用户已明确要求这次变更范围不仅包括 `m16n8k16 row.col f16->f16`，还包括
`m16n16k16 row.col f16->f16`。后者在 active MMA architecture 中本就属于 committed
`split-n` composite family，只是尚未被 current contract 接通。因此，本 change 需要把这两条
`fp16 -> fp16` family 作为同一组 current-support 增量来收敛。

## What Changes

- 新建 active change `support-fp16-fp16-mma`，专门负责把当前 blocked 的
  `row.col fp16 -> fp16` MMA family 从“显式 fail-fast”推进为 current supported contract。
- 本 change 的实现范围限定为：
  - direct-native `m16n8k16 row.col f16 -> f16`
  - committed `split-n` composite `m16n16k16 row.col f16 -> f16`
- 以 `lab/07_fp16_mma_ptx_probe/` 的已验证结论为设计输入，明确 sbtsim 主线必须采用
  “Ventus window / logical tile / PTX native fragment”分层建模，而不是继续把旧 blocked 路径中的
  shape-only metadata 或未验证 tuple 直连方案当作 current contract。
- 明确本 change 需要同时收敛两类问题：
  - current contract：把 `inst-support`、README、实现地图和 oracle 入口从 blocked 口径改成 supported 口径；
  - lowering contract：在 `sbt` 主线中选定并实现一条对 Spike / CPU ref 可验证的 PTX operand 供给方案。
- 明确 archived `support-custom-mma` change 中关于 `fp16 -> fp16` 的 blocked 口径只作为 historical background 保留；
  当前是否继续 blocked，应由本 change 的 proposal/design/spec 决定，而不再默认沿用临时 note。

## Capabilities

### New Capabilities

- **Current fp16 MMA support on PTX backend**：为 `m16n8k16 row.col f16 -> f16` 提供 current supported PTX lowering 与 semantic validation contract。
- **Current fp16 MMA split-n support**：为 `m16n16k16 row.col f16 -> f16` 提供 current supported committed `split-n` composite lowering 与 semantic validation contract。
- **Repository-local fp16 MMA physical-fragment contract**：把 `Ventus raw window / logical tile / PTX native fragment` 的受支持映射方式写成可验证 contract，而不是继续依赖 historical blocked 假设。

### Modified Capabilities

- **inst-support**：`fp16 -> fp16` 不再整体视作 blocked exception；改为把 `m16n8k16 row.col` 与
  `m16n16k16 row.col split-n` 纳入 current landed MMA subset，其它未支持 `fp16 -> fp16` 组合仍保持显式 fail-fast。
- **MMA semantic oracle boundary**：当前 MMA semantic gate 需要从“只覆盖已落地非-`fp16->fp16` 子集”扩展为包含上面两条 `fp16 -> fp16` family，并以 CPU reference / Spike / PTX 的一致性作为主验证口径。

## Impact

- current MMA support surface 会从“首批 `row.col` MMA 子集但排除全部 `fp16 -> fp16`”扩大为：
  - direct-native：额外包含 `m16n8k16 row.col f16 -> f16`
  - committed split-n：额外包含 `m16n16k16 row.col f16 -> f16`
- current `unsupported.mma.fp16_fp16_contract_pending` fail-fast 将不再适用于这两条 family；但其它
  非 current `fp16 -> fp16` family 仍必须继续显式 fail-fast。
- `lab/07_fp16_mma_ptx_probe/` 中的实验证据需要被正式吸收为 design 输入，而不是继续停留在 isolated experiment。
- archived `support-custom-mma` 中关于 `fp16 -> fp16` blocked 的临时 note 不会被删除，但其结论会被本 change supersede：
  historical note 继续保留“当时为何 blocked”的背景，current contract 则由本 change 更新。

## Documentation Impact

本 change 落地时至少需要检查并更新以下文档与 specs：

- `README.md`
- `doc/README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `doc/mma/LOWERING_ARCHITECTURE.md`
- `tools/README.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`

本 change supersedes archived `support-custom-mma` change 中“`fp16 -> fp16` 仍必须维持 blocked”的
historical implementation conclusion，但不 supersede 该 archived change 的其它 landed MMA matrix / architecture material。
