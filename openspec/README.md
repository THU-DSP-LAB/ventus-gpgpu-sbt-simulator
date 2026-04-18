# openspec/

本目录用于维护两类内容：

- `openspec/specs/`：当前仍然生效、应被当作 contract 阅读的能力规格
- `openspec/changes/`：进行中的 change；完成后归档到 `openspec/changes/archive/`

## 阅读顺序

若你要理解当前仓库的 OpenSpec 口径，建议按这个顺序读：

1. `openspec/project.md`
2. `openspec/specs/global-address-space/spec.md`
3. `openspec/specs/replicated-scalar-state/spec.md`
4. `openspec/specs/ptx-call-prototype/spec.md`
5. 其它当前 specs

## 状态分层

- `current`：当前维护中的规格，默认位于 `openspec/specs/`
- `active`：进行中的变更，位于 `openspec/changes/<name>/`
- `historical`：已完成并归档的变更，位于 `openspec/changes/archive/`
- `legacy`：保留背景价值但已不再是当前主线的规格；必须在文档开头显式标注

## 当前 specs

- `build-time-spike-pattern-subset`：构建期 Spike pattern 子集生成合同
- `global-address-space`：当前 PTX ordinary address mapping / single-Global / VMM backing 合同
- `inst-support`：指令支持与语义验证合同（当前已包含 landed 的 non-MMA 子集与首批 committed `row.col` MMA 子集；Spike-backed 非 custom 指令当前通过共享 instruction metadata 维护 operand / immediate / uniform-transfer contract；所有 current supported MMA family 现统一采用 `Spike / sbtsim PTX / CPU reference` 三方语义验证）
- `ptx-call-prototype`：多函数 PTX helper prototype / value ABI 合同
- `ptx-temp-register-allocation`：当前 PTX emitter 固定槽位与 `%tmp*` scratch ownership 合同
- `replicated-scalar-state`：当前 PTX lowering 主线合同（包含 scalar execution classification 显式化、未分类 scalar 默认拒绝的 current contract）
- `sbt-rodinia-bringup`：Rodinia bring-up / fail-fast 边界

## 当前 active changes

- `reduce-lowering-name-dependence`：当前唯一 active change；目标已收紧为“让 current supported emit path 整体转成 descriptor-driven”。`DecodedInst.name` 继续保留为 external mnemonic contract，但不再作为 PTX emitter correctness path 的 authority。该 change 要求 ordinary/custom/MMA 在进入 emitter 前都具备 emit-authoritative descriptor / payload，并同步把 CFG control-flow 分类收口到共享前置 descriptor；`cfg_verify` 的更宽结构化规则重写仍不在本 change 范围内。

当前口径提醒：
- `current` 的固定 machine/runtime/control 槽位真相以 `doc/IMPLEMENTATION_CODEMAP.md`、`sbt/ptx_emit.cpp` 与 `openspec/specs/ptx-temp-register-allocation/spec.md` 为准。
- PTX emitter 当前 scratch ownership 已收敛到函数级唯一命名 `%tmp*` 寄存器；旧的 `%r14/%r15/...` 隐式共享 scratch 约定只保留为 historical 背景，不再作为 current contract。

## 近期 historical changes（与 custom split 相关）

- `openspec/changes/archive/2026-04-16-tighten-instruction-metadata-contract`：已归档的 instruction metadata contract 收敛 change；其结果已同步进 current `inst-support` / `replicated-scalar-state` specs、README/doc 索引与相关 decode / CFG verify / emitter 回归。
- `openspec/changes/archive/2026-04-14-tighten-mma-triple-oracle-regression`：已归档的 MMA 三方 oracle 收敛 change；其结果已同步进 current `inst-support` spec、README 与统一回归入口。
- `openspec/changes/archive/2026-04-14-support-fp16-fp16-mma`：已归档的 `fp16 -> fp16` MMA change；其结果已同步进 current `inst-support` spec、统一回归入口与相关 current 文档。`lab/07_fp16_mma_ptx_probe/` 保留为该 change 的 historical 前期实验记录。
- `openspec/changes/archive/2026-04-14-introduce-ptx-virtual-temp-registers`：已归档的虚拟临时寄存器 change；其结果已同步进 current `ptx-temp-register-allocation` spec、README/doc 索引与 PTX emitter 回归。
- `openspec/changes/archive/2026-04-13-support-custom-mma`：已归档的 MMA change；其结果已同步进 current spec，形成当前 landed 首批 `row.col` MMA contract。其时对 `fp16 -> fp16` 的 blocked 结论现在仅作为 `historical` 背景。
- `openspec/changes/archive/2026-04-05-support-custom-instructions`：已归档的 non-MMA custom change（historical）。

custom 指令与普通 lowering 去名字耦合主题当前有一个未归档 change：`reduce-lowering-name-dependence`。若后续继续推进 blocked/deferred/research MMA family，应新开 change 承载增量 contract，而不是混入该 change。

## Legacy specs

- `simple-ptx-prototype`：仅保留最早期最小样例背景，不再作为当前实现与回归基线

## 维护约定

- 新增或改变 current contract 时，优先新增/更新 change，再同步到 `openspec/specs/`
- 纯文档收敛若不改变 contract，可直接更新 `project.md`、`config.yaml`、索引与状态说明
- 任何已完成 change 都不应长期停留在 `openspec/changes/`；应同步 spec 或归档
- `README.md`、`doc/README.md`、`openspec/README.md` 的导航必须保持一致
