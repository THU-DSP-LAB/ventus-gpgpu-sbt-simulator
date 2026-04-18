## Implementation Tasks

## 1. Freeze the whole-emitter migration boundary

- [ ] 1.1 清点 `sbt/ptx_emit.cpp` 中所有 current supported 的 `di.name` 语义分派点，并按语义域分组：
  - control-flow / call / ret / structured control
  - scalar memory / scalar integer / scalar FP / CSR
  - vector memory / PDS
  - vector integer / compare / mask / move / convert
  - vector FP / compare / convert
  - custom non-MMA
  - MMA（确认现有结构化 contract 已满足目标）
- [ ] 1.2 在 change 目录下维护一份 checked-in inventory / allowlist，记录每个 name-dependent site 的语义域、替代 descriptor / payload、以及迁移后残余允许保留的 `name` 用法；其中 decode / CFG site 需显式标注为 deferred，不计入本 change 完成条件。
- [ ] 1.3 为所有 current supported emit path 冻结 emitter-facing descriptor 分层，明确每个语义域需要的最小字段。
- [ ] 1.4 明确 descriptor 的唯一归属与注入点：只能来自 `DecodedInst` 字段或 decode 过程中消费的 repository-managed shared metadata，禁止在 emitter / emitter-local classification helper 内新建 consumer-local 语义推断层。

## 2. Introduce the emitter-facing descriptor layer

- [ ] 2.1 为 control-flow 引入显式 descriptor，至少覆盖：
  - branch kind
  - branch condition kind
  - direct jump / direct call / ret / indirect terminator 分类
  - `setrpc/join/barrier/vsetvli/endprg` 的 structured control kind 与当前 supported emitter 语义
- [ ] 2.2 为 scalar memory 引入 descriptor，覆盖 load/store kind、width、sign-extension/raw-bit 语义和执行模式要求。
- [ ] 2.3 为 scalar integer / bitmanip 引入 descriptor，覆盖 op kind、operand flavor 和 signed/unsigned/shift 语义。
- [ ] 2.4 为 scalar FP 引入 descriptor，覆盖 move/sign/minmax/cmp/convert/class/fma/unary-binary 等 lowering 语义与 rounding-mode 使用规则。
- [ ] 2.5 为 CSR lowering 引入 descriptor，覆盖 csr op kind 与 current supported CSR semantic class。
- [ ] 2.6 为 scalar-side 引入显式 domain / tag，覆盖“是否需要 scalar execution classification”这条 applicability boundary，不再依赖 emitter-local 名字例外。
- [ ] 2.7 为 vector memory / PDS 引入 descriptor，覆盖 ordinary memory vs PDS addressing kind、width、sign-extension、store kind。
- [ ] 2.8 为 vector integer / compare / mask / move / convert / FP lowering 引入 descriptor，覆盖 domain kind、operand flavor、compare semantics、result encoding、convert semantics。
- [ ] 2.9 确认 MMA 现有 `MmaInstInfo + planner / ABI descriptor` 路线在统一 emitter contract 下的 ownership 与入口，不把它退化回名字分派。

## 3. Convert emitter control-flow / structured-control emission away from name-driven dispatch

- [ ] 3.1 让 emitter 的 scalar/vector branch、direct jump/call/ret/indirect terminator lowering 消费显式 control descriptor。
- [ ] 3.2 让 emitter 对 `setrpc/join/barrier/vsetvli/endprg` 的当前 supported 语义也消费显式 structured-control descriptor，而不是名字分支。
- [ ] 3.3 在文档与 inventory 中显式标注：`sbt/cfg.cpp` / `sbt/cfg_verify.cpp` 的去 name string 化不属于本 change，留给后续 change。

## 4. Convert scalar-side emission away from name-driven dispatch

- [ ] 4.1 让 scalar load/store lowering 只消费 scalar memory descriptor，不再重新匹配 `lw/lb/lh/lbu/lhu/sw/sb/sh/flw/fsw` 名称。
- [ ] 4.2 让 scalar integer / bitmanip lowering 只消费 scalar integer descriptor，不再由 `addi/add/sub/...` 名称树决定语义分支。
- [ ] 4.3 让 scalar FP lowering 只消费 scalar FP descriptor，不再由 `fadd_s/fsgnj_s/fcvt_*` 等名字树决定 lowering 路径。
- [ ] 4.4 让 CSR lowering 只消费 CSR descriptor，不再由 emitter 内部名字分支决定 CSR op 语义。
- [ ] 4.5 保持 `scalar_exec_kind` 继续作为 execution classification authority，并确保 scalar-side descriptor 与 execution classification 一起决定 lowering。
- [ ] 4.6 让“是否需要 scalar execution classification”的判断也消费显式 metadata / domain tag，不再由 `jal/jalr/vmv_x_s/...` 名字例外决定 applicability。

## 5. Convert vector ordinary emission away from name-driven dispatch

- [ ] 5.1 让 vector ordinary memory / PDS lowering 只消费 vector memory descriptor。
- [ ] 5.2 让 vector integer arithmetic / compare / move / convert lowering 只消费 vector ordinary descriptors，而不是 `vadd_* / vsub_* / vmseq_* / ...` 名字树。
- [ ] 5.3 让 vector mask boolean / compare-result lowering 只消费显式 descriptor，不再依赖 `vmand_mm/vmor_mm/...` 名字分类。
- [ ] 5.4 让 vector FP / FP compare / FP convert lowering 只消费 vector FP descriptors，而不是 `vf*` 名字树。

## 6. Make custom non-MMA fully payload-authoritative

- [ ] 6.1 让 `shuffle` lowering 只消费 `custom.family/subop` 与已解码操作数字段，不再按 `shuffle_idx/up/down/bfly` 名字分派。
- [ ] 6.2 让 `vcvt` lowering 只消费 `custom.family/subop/dtype` 与已解码操作数字段，不再按 `vcvt_*` 名字分派。
- [ ] 6.3 让 packed arithmetic lowering 只消费 `custom.family/subop/dtype`，不再按 `vadd/vmul/vfma + f16x2/bf16x2` 名字分派。
- [ ] 6.4 让 SFU lowering 只消费 `custom.family/subop/dtype`，删除 `name -> subop` 与 `name -> dtype` 的语义回退路径。
- [ ] 6.5 对 current supported custom non-MMA family，缺 payload 时显式失败，不允许在 emitter 内回退到 mnemonic 解析。

## 7. Reorganize emitter dispatch structure

- [ ] 7.1 按 semantic domain / kind 重组 `sbt/ptx_emit.cpp` 的主 lowering 入口，去掉 supported path 的大号字符串分派树。
- [ ] 7.2 明确并保留 `DecodedInst.name` 的允许使用范围，仅限：
  - pretty / JSON / diagnostics / coverage
  - comments / debug text
  - external builtin symbol / reporting 场景
- [ ] 7.3 确认 supported emit path 的 correctness branch 中不再以 `di.name` 作为 lowering authority。
- [ ] 7.4 把允许读取 `DecodedInst.name` 的残余位置收敛成显式 allowlist，并与 1.3 的 inventory / allowlist 保持同步。

## 8. Regression and validation

- [ ] 8.1 增加或更新 decode / metadata contract tests，覆盖所有 emitter-facing descriptor / payload 的填充路径。
- [ ] 8.2 增加或更新 control / structured-control descriptor contract tests，覆盖 emitter 输入所需字段的填充与消费路径。
- [ ] 8.3 增加或更新 emitter / compile-first tests，覆盖 scalar / vector / custom / MMA 各语义域的 descriptor-driven lowering。
- [ ] 8.4 增加负向测试：supported-path 指令缺 descriptor / payload 时显式失败，而不是回退到 `name`。
- [ ] 8.5 增加静态结构性检查：验证 `sbt/ptx_emit.cpp` 及相关 emitter helper 中允许读取 `di.name` 的位置只剩 allowlist 中的 external / diagnostic / comment / reporting 用途。
- [ ] 8.6 增加动态 poison-name 检查：在 representative supported-path 测试里故意污染 `DecodedInst.name`，验证 lowering 仍只由 descriptor / payload 决定。
- [ ] 8.7 增加 external mnemonic contract 回归，覆盖至少：
  - `sbt_decode pretty`
  - JSON / diagnostics
  - coverage / mnemonic reporting
  - ABI-visible builtin symbol / external reporting 场景
- [ ] 8.8 运行受影响的现有回归，至少包括：
  - `instruction_metadata_contract_test`
  - `custom_decode_test`
  - `custom_ptx_emit_test`
  - `mma_ptx_emit_test`
  - `ptx_emit_leader_lane_abi_test`
  - 任何为 descriptor contract 新增的专项测试
- [ ] 8.9 运行现有 semantic oracle，确认 ordinary/custom/MMA 的可观察语义不被重构改变。

## 9. Documentation sync

- [ ] 9.1 更新 `README.md`，明确区分 current behavior 与本 change target behavior。
- [ ] 9.2 更新 `doc/IMPLEMENTATION_CODEMAP.md`，记录 current as-built emitter 与 target descriptor-driven emitter 之间的边界，并在落地后同步 ownership 真相。
- [ ] 9.3 更新 `doc/README.md`，确保该主题只有一个 active 入口，不把归档讨论稿继续当作当前入口。
- [ ] 9.4 更新 `openspec/README.md` 与受影响 current specs，保持 `current` / `active` / `historical` / `legacy` 标签和索引口径一致。

## 10. Final consistency check

- [ ] 10.1 复查本 change 的实际落地范围，确认目标仍是“emit 整体 descriptor-driven”，而不是退回旧方案的局部 first-stage 收口。
- [ ] 10.2 确认没有把 `inst_id = hash(name)` 包装成已完成的 opcode 重构。
- [ ] 10.3 确认 supported emit path 中不存在新的隐式 fallback，也不存在残留的 name-driven correctness branch。
- [ ] 10.4 确认本 change 没有被错误扩张成 decode / shared-metadata lookup / CFG 的全链路去 name string 化；这些范围必须继续标注为 deferred。
- [ ] 10.5 确认 checked-in inventory / allowlist 中 emitter supported-path 站点已经清零，只剩被显式批准的 external / diagnostic / reporting 用法；decode / CFG 站点若存在，必须明确标为 deferred。
- [ ] 10.6 确认 external mnemonic contract 回归仍通过，没有因 authority 迁移破坏 pretty / JSON / coverage / diagnostics 口径。
- [ ] 10.7 校对所有相关文档与 spec 的状态标记，确保 current 入口、active change 与 archive 材料口径一致。
