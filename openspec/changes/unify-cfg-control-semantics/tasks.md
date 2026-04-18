## Implementation Tasks

## 1. Shared control-semantics helper

- [ ] 1.1 新增共享 control-semantics helper，统一从 `DecodedInst` 的结构化字段读取 branch / jump / call / return / indirect terminator / structured control 语义。
- [ ] 1.2 在 helper 中为 supported-path 控制流语义缺失、类型不匹配或 contract 自相矛盾的情况建立显式 fail-fast 诊断。
- [ ] 1.3 明确 helper 与 `EmitDescriptor` / ordinary metadata 的边界，避免在消费者侧重新引入一份本地语义表。

## 2. CFG and verify migration

- [ ] 2.1 改造 `sbt/cfg.cpp`，让 leader / terminator / edge 分类只消费共享 control-semantics helper，不再按 `di.name` 分类 `vb*` / `jal` / `jalr` / `join` / `endprg`。
- [ ] 2.2 改造 `sbt/cfg_verify.cpp`，让 `setrpc` / `vbranch` / `join` / `barrier` / unsupported `jalr` 判定只消费共享控制流语义。
- [ ] 2.3 将 `cfg_verify` 中 `auipc` 回溯识别切到 shared metadata / descriptor authority，而不是 mnemonic 比较。

## 3. Main-pipeline call-graph migration

- [ ] 3.1 改造 `tools/sbt_ptx.cpp` 的 direct-call 闭包扫描，让 direct-call 识别只消费 `ControlKind::DirectCall`。
- [ ] 3.2 确认 main pipeline 中不再残留 supported-path `di.name == \"jal\"` 之类的控制流 authority 判断。

## 4. Regression coverage

- [ ] 4.1 扩充或新增 regression，覆盖 poison-name 不改变 CFG build / CFG verify / direct-call scan 结果。
- [ ] 4.2 扩充或新增 regression，覆盖结构化控制流字段缺失时的显式 fail-fast 行为。
- [ ] 4.3 扩充或新增 regression，覆盖 regext-bundled control-flow instruction 的 `bundle pc` / `inst_pc` 语义保持不变。
- [ ] 4.4 运行并记录至少以下受影响回归：相关 contract test、CFG/verify 测试、以及 `sbt_ptx` 主流程 smoke。

## 5. Documentation sync

- [ ] 5.1 在实现落地后更新 `README.md` 与 `doc/IMPLEMENTATION_CODEMAP.md`，把 current as-built 真相切换到新的 control-semantics authority；在本 change 仍为 `active` 时不提前改写 current 文档事实。
- [ ] 5.2 更新 `doc/README.md`、`openspec/README.md` 与受影响 current specs，反映新的 active change 与 current contract 边界。

## 6. Final consistency check

- [ ] 6.1 校对 current / active / historical / legacy 标签与导航引用，确认没有把 archive 材料继续写成当前入口。
- [ ] 6.2 复查本 change 的实际落地边界，确认它只收口 main-pipeline control semantics authority，没有无意扩张成更大范围的 descriptor 重构。
