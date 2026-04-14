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
- `inst-support`：指令支持与语义验证合同（当前已包含 landed 的 non-MMA 子集与首批 committed `row.col` MMA 子集，包括 current supported `fp16 -> fp16` family）
- `ptx-call-prototype`：多函数 PTX helper prototype / value ABI 合同
- `replicated-scalar-state`：当前 PTX lowering 主线合同
- `sbt-rodinia-bringup`：Rodinia bring-up / fail-fast 边界

## 当前 active changes

当前没有 active change。

## 近期 historical changes（与 custom split 相关）

- `openspec/changes/archive/2026-04-14-support-fp16-fp16-mma`：已归档的 `fp16 -> fp16` MMA change；其结果已同步进 current `inst-support` spec、统一回归入口与相关 current 文档。`lab/07_fp16_mma_ptx_probe/` 保留为该 change 的 historical 前期实验记录。
- `openspec/changes/archive/2026-04-13-support-custom-mma`：已归档的 MMA change；其结果已同步进 current spec，形成当前 landed 首批 `row.col` MMA contract。其时对 `fp16 -> fp16` 的 blocked 结论现在仅作为 `historical` 背景。
- `openspec/changes/archive/2026-04-05-support-custom-instructions`：已归档的 non-MMA custom change（historical）。

当前 custom 指令主题已经没有未归档的 active change；若后续继续推进 blocked/deferred/research MMA family，应新开 change 承载增量 contract。

## Legacy specs

- `simple-ptx-prototype`：仅保留最早期最小样例背景，不再作为当前实现与回归基线

## 维护约定

- 新增或改变 current contract 时，优先新增/更新 change，再同步到 `openspec/specs/`
- 纯文档收敛若不改变 contract，可直接更新 `project.md`、`config.yaml`、索引与状态说明
- 任何已完成 change 都不应长期停留在 `openspec/changes/`；应同步 spec 或归档
- `README.md`、`doc/README.md`、`openspec/README.md` 的导航必须保持一致
