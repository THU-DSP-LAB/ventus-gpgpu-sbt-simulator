## Implementation Tasks

## 1. Shared instruction metadata

- [x] 1.1 为当前 supported subset 引入最小共享 `InstId + InstMetadata` contract，至少覆盖 `operand_form`、`imm_kind`、`uniform_transfer_kind`，并保留 `DecodedInst.name` 仅用于诊断与现有输出兼容。
- [x] 1.2 让 `sbt/riscv_decode.*` 在 scalar decode 与 Spike-backed pattern decode 路径中填充共享 metadata，而不是继续以 mnemonic suffix 作为主要 operand / immediate 推断来源。
- [x] 1.3 保持 custom non-MMA / MMA 的显式 metadata 路径不回退，并确保它们与新的共享 metadata contract 共存而不是互相覆盖。
- [x] 1.4 明确并落实“新增 Spike-backed 指令”的同步入口：除 metadata 外，还必须同步 `data/spike_want.txt` / build-time pattern subset 依赖链，避免产生不可达的 metadata 条目。

## 2. CFG verify convergence

- [x] 2.1 改造 `sbt/cfg_verify.*`，让 vector uniform transfer 传播消费共享 metadata，而不是维护独立的 `_vx/_vi/_vv/_v` suffix 推断逻辑。
- [x] 2.2 对 metadata 缺失或不完整的 supported-path 指令显式失败，禁止 CFG verify 静默回退到字符串猜测。

## 3. Scalar execution classification hardening

- [x] 3.1 把 PTX emitter 的 scalar execution classification 改成显式、完整、默认拒绝未分类项的实现。
- [x] 3.2 为当前 supported scalar subset 逐条声明 `uniform-pure` / `lane-sensitive` / `fixed-lane-sensitive` / `externally-side-effecting` 分类，移除“查不到就当 `UniformPure`”的路径。
- [x] 3.3 保持 current contract 中已验证的行为不变，包括 replicated scalar-state 下的 scalar branch、`vmv.x.s` fixed-lane 语义，以及 leader-only side-effect lowering。

## 4. Regression coverage

- [x] 4.1 扩充或更新 decode / verify 相关单元测试，覆盖共享 metadata 而不是 suffix 规则成为事实源的行为。
- [x] 4.2 扩充或更新 `ptx_emit_leader_lane_abi_test` 及相关 emitter 回归，覆盖“未分类 scalar 必须显式失败”与“现有 current subset 行为不变”。
- [x] 4.3 运行并记录至少以下回归：`ptx_emit_leader_lane_abi_test`、相关 decode/verify 单测、`custom_decode_test`、`mma_decode_test`，以及任何因 metadata 收敛新增的最小专项测试。

## 5. Documentation sync

- [x] 5.1 同步更新 `README.md`、`doc/IMPLEMENTATION_CODEMAP.md`，把 decode / verify / emitter 的 instruction metadata current 口径写清楚。
- [x] 5.2 同步更新 `doc/README.md`、`openspec/README.md` 与受影响的 current specs，明确本 change 的 current / active / historical 口径，修正索引中与实际状态不一致的 active change 描述。

## 6. Final consistency check

- [x] 6.1 校对本 change 与 current specs 的边界，确认“共享 metadata + scalar 默认拒绝未分类项”已纳入 current contract，而“semantic IR / emitter 全量 dispatch 重构 / custom+MMA 统一大抽象”仍明确处于本 change 范围之外。
- [x] 6.2 复查 `current` / `active` / `historical` / `legacy` 标签与导航引用，确保没有把 archive change 或历史设计稿继续写成当前进行中的入口。

## Regression Notes

- 2026-04-15：`./build/instruction_metadata_contract_test` -> `ok instruction metadata contract`
- 2026-04-15：`./build/ptx_emit_leader_lane_abi_test` -> `ok ptx replicated scalar state`
- 2026-04-15：`./build/custom_decode_test` -> `ok custom decode path`
- 2026-04-15：`./build/mma_decode_test` -> `ok mma decode path`
- 2026-04-15：`./build/ptx_emit_call_prototype_test` -> `ok ptx helper call prototype`
