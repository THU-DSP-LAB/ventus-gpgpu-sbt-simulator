## Why

当前 `sbt/ptx_emit.cpp` 的 correctness path 仍大面积依赖 `DecodedInst.name` 做 lowering 分派。这个问题已经不只是 custom non-MMA 或少量 ordinary 指令的局部技术债，而是整个 emitter 的主结构问题：

- scalar branch / load-store / ALU / FP / CSR 仍以名字字符串决定 lowering 分支；
- vector memory / integer / float / compare / mask / convert / move 也大面积以名字字符串决定 lowering 分支；
- custom non-MMA 虽已具备 `family/subop/dtype` payload，但 emitter 仍残留按 mnemonic stem/suffix 选分支或做语义回退的路径；
- MMA 已经基本走结构化 metadata + planner / ABI descriptor 路线，反而与 ordinary/custom non-MMA 的主线形态不一致；
- `DecodedInst.name` 仍同时承担 external mnemonic contract 与 emitter semantic authority，导致 rename / alias / metadata 扩展时容易形成多处事实源。

在这种结构下，继续沿用旧方案里的“局部高价值 first-stage descriptor 收口”只能减轻局部痛点，不能达成真正的 contract 收敛。若目标是让 emitter 整体走向稳定、可维护、可验证的结构，那么必须把目标明确改成：

- **所有 current supported emit path（包括当前作为 no-op / barrier / return-like 处理的 structured control 指令）都依赖 decode / shared metadata 产出的显式 descriptor 或 payload 进行 lowering**
- **`DecodedInst.name` 保留为 external mnemonic contract，但不再参与 emitter correctness path**

仓库里已有讨论纪要 `doc/archive/TEMP_LOWERING_OPCODE_AND_NAME_EXCHANGE_FORMAT_DISCUSSION.md`。本 change 的作用不再是沿用旧方案、从中挑一小段 first-stage 范围落地，而是把已经收敛出来的核心方向固化成更完整的 active change：

- `name` 的职责边界；
- emitter-authoritative descriptor / payload 的唯一事实源要求；
- ordinary/custom/MMA 在 target contract 下的统一 emitter-facing 结构；
- 全 emitter 去字符串化的实施顺序、验证要求与文档同步要求。

## What Changes

- 把 target contract 改成：**所有 current supported、可到达 PTX emission 的指令**，都必须在进入 emitter 前具备足够的 emit-authoritative descriptor / payload；这也包括当前在 emitter 中以 no-op / barrier / return-like 语义处理的 structured control 指令。
- 明确本 change 的目标是：`DecodedInst.name` 不再作为 **PTX emitter correctness path** 的 authoritative semantic source；但本 change 不要求 decode / shared-metadata lookup / CFG 立刻完成去 name string 化。
- 为 ordinary non-custom 指令扩展 descriptor 目标范围，不再局限于旧方案的 first-stage 最小集；至少覆盖：
  - control-flow / call / return / indirect terminator / structured control 语义
  - scalar load/store 语义
  - scalar integer / scalar FP / CSR lowering 语义
  - vector memory / vector integer / vector FP / compare / mask / convert / move 等 ordinary vector lowering 语义
- 把 custom non-MMA target contract 收紧为 payload-authoritative：
  - `custom.family / custom.subop / custom.dtype` 与显式操作数字段成为 lowering authority；
  - 删除 emitter 内 `name -> subop/dtype/family` 的语义回退路径。
- 保留 MMA 当前 `MmaInstInfo + planner / ABI descriptor` 路线，但把它纳入“已满足结构化 emit contract 的现有正例”，而不是与 ordinary/custom 并列游离的特殊路线。
- 允许为 emitter 引入所需的 control / structured-control descriptor 或 shared metadata；但本 change 不要求 `sbt/cfg.cpp`、`sbt/cfg_verify.cpp` 或 decode metadata lookup 同步完成去 name string 化，这部分留给后续 change。
- 要求任何 current supported emit path 在缺少必要 descriptor / payload 时都显式失败，而不是在 emitter 内回退到 `DecodedInst.name` 解析。
- 要求“是否需要 scalar execution classification”这条边界本身也来自显式 metadata / domain tagging，而不是来自 emitter-local 名字例外。
- 把验证要求收紧为三条并行目标：
  - 语义保持不变；
  - supported emit path 的 lowering authority 不再来自 `DecodedInst.name`；
  - `DecodedInst.name` 的 external mnemonic contract 继续通过 pretty / JSON / diagnostics / coverage 等路径稳定暴露。
- 要求在 change 中维护一份 checked-in inventory / allowlist，记录每个 name-dependent site 的归属语义域、替代 descriptor 与残余允许用法，并作为每个 phase 的退出条件之一。
- 要求在 proposal / design / tasks / spec 里显式标注 deferred boundary：
  - 当前 change 主要完成 emitter correctness path 去字符串化；
  - decode / shared-metadata lookup / CFG 的去 name string 化属于后续 change。
- 把“文档检查/修改”纳入任务，要求落地时同步更新 `README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`doc/README.md`、`openspec/README.md` 和受影响 current specs。

本 change 明确不做：

- 不把 `inst_id = hash(name)` 包装成真正 opcode 体系；
- 不修改上级 `ventus-env` 子项目源码；
- 不要求引入一个独立于 `DecodedInst` 之外的全新 IR 图结构；但允许扩展 `DecodedInst` / shared metadata，使其足以承载 emit-authoritative descriptor；
- 不要求在本 change 中移除 decode / shared metadata lookup 对 mnemonic name 的内部依赖；
- 不要求在本 change 中重写 `sbt/cfg.cpp` / `sbt/cfg_verify.cpp` 的名字分类或结构化分析逻辑；
- 不把 `cfg_verify` 围绕 `setrpc/join/barrier` 的全部结构化规则都重写成 descriptor-first。

## Capabilities

### New Capabilities

- 新增“emit-authoritative lowering descriptor contract”能力：所有 current supported emit path 在进入 PTX emission 前，都必须具备足够的结构化 descriptor / payload 来决定 lowering 语义。
- 新增“descriptor-driven PTX emission contract”能力：PTX emitter 的 correctness path 只消费 decode / shared metadata 提供的 descriptor / payload，而不从 `DecodedInst.name` 二次推断 ordinary/custom 语义。

### Modified Capabilities

- 修改 `inst-support` current contract：当前支持面不仅要求“能 decode / 能 compile / 能通过语义 oracle”，还要求 ordinary/custom/MMA 的 emit 语义 authority 在 PTX emission 前已经结构化、可被共享消费。
- 修改 `replicated-scalar-state` current contract：scalar-side lowering 的 control-flow / load-store / scalar ALU / scalar FP / CSR 等 emit 决策必须由显式 descriptor / classification 驱动，而不是由 emitter 内部名字匹配驱动。

## Impact

- Affected areas:
  - `sbt/riscv_decode.*`
  - `sbt/instruction_metadata.*`
  - `sbt/cfg.*`
  - `sbt/ptx_emit.*`
  - ordinary/custom/MMA 相关 decode metadata、PTX emission 与回归
  - current 文档与 OpenSpec specs
- Expected outcome:
  - `DecodedInst.name` 与 PTX emission correctness path 脱钩，但不影响 pretty/JSON/coverage/diagnostics contract；
  - 当前 change 完成后，decode / shared-metadata lookup / CFG 内部仍可能保留 name-string 依赖；这属于显式 deferred work，而不是本次未声明的遗漏；
  - `sbt/ptx_emit.cpp` 的主分派结构从字符串树转为 descriptor domain / kind 驱动，当前 structured control 语义也纳入显式 control descriptor；
  - custom non-MMA 与 ordinary non-custom 不再维持“decode 有结构化信息、emit 仍按字符串落分支”的双事实源；
  - scalar execution classification 的 applicability boundary 也转为 metadata / domain 驱动，而不是名字例外驱动；
  - MMA 继续作为当前已经结构化的正例，并在统一 emitter contract 下获得更清晰的角色定位；
  - 评审与回归可以同时依赖 checked-in inventory、静态 allowlist 检查、动态 poison-name 测试以及 external mnemonic contract 回归来判断 change 是否完成；
  - 后续若引入 generated `Opcode enum`，也只能建立在 descriptor 已成为 authority 的前提上，而不是重新退化回大号名字/枚举 switch。

### Documentation Impact

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `doc/README.md`
- `openspec/README.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`

### Superseded Discussion Notes

- 本 change 吸收并 supersede `doc/archive/TEMP_LOWERING_OPCODE_AND_NAME_EXCHANGE_FORMAT_DISCUSSION.md` 中已经收敛清楚的 active 结论；该归档文档继续保留为 `historical` 背景，不再作为后续实现的唯一入口。
