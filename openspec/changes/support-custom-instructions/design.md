## Context

本设计描述的是 **target behavior**，不是当前已实现行为。

本 change 依赖的 canonical current docs/specs 有：

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/project.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/specs/build-time-spike-pattern-subset/spec.md`

本 change 同时依赖一份跨两个 active changes 的 shared active baseline：

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`

当前实现的关键边界是：

- decode 主要依赖 Spike pattern 子集；不存在于 `encoding.h` 的新 custom 指令没有正式入口；
- `DecodedInst` 只能表达通用寄存器和立即数，不能直接承载 `shape/layout/dtype/subop` 之类的 custom 元数据；
- PTX emitter 当前固定较低 PTX 版本，仓库默认 compile-first / regress 也仍以 `sm_75` 为例；
- 项目 domain context 明确 `v0` 是普通向量寄存器；non-MMA custom 指令里的 `vm`/`m` 编码位当前也不引入独立 mask 语义；
- 本地 `ptxas 13.1` 探针已证明：
  - `shuffle` 和 `f16x2` packed 算术可以直接映射；
  - `bf16x2` packed 算术要求更高目标架构；
  - `vcvt.*f16x2/bf16x2` 可以直接映射；
  - packed SFU 并不是所有子操作都有原生 PTX；
  - MMA 的部分 Ventus shape 不能直接当作 PTX native MMA shape 使用。

这意味着把 non-MMA 和 MMA 拆成两个 changes 是合理的；但当前这个 non-MMA change 仍然不是简单把 emitter case 补全，它还是要先解决规范、解码架构、IR、以及 PTX baseline 四个基础问题。

另外，PTX baseline、semantic oracle、packed 语义与 MMA 首发边界都不再由当前 change 单独拍板，而是先冻结在 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中，再进入任一 custom change 的实现阶段。

## Goals / Non-Goals

**Goals:**

- 把 `doc/CUSTOM_INSTRUCTION_INPUT.md` 中的新增指令收敛到一个可实施的 active change，而不是继续依赖未同步的输入材料口述。
- 将 custom 指令支持拆分为两个 active changes，并把本 change 收敛为 non-MMA 范围。
- 为 custom 指令建立可持续的前端架构：允许 Spike-backed pattern 与 repo-local custom decode 并存。
- 在 shared baseline 文档已冻结的 `sm_90` / oracle / packed 合同之上实现 non-MMA custom 支持，避免为了兼容旧 SM 而把大量新指令降成复杂软件模拟。
- 为后续 MMA change 预留可扩展的前端结构，但不在本 change 中承担 MMA lowering 责任。

**Non-Goals:**

- 不把 `doc/CUSTOM_INSTRUCTION_INPUT.md` 直接当作 current contract；它只作为输入材料，active/current contract 以本 change 与后续 synced specs 为准。
- 不保留 `sm_75` 兼容性作为本 change 的目标。
- 不在本 change 中实现 MMA lowering、MMA support matrix 或 MMA semantic validation。

## Decisions

### 1. Canonical source of truth moves from raw Markdown to OpenSpec artifacts

`doc/CUSTOM_INSTRUCTION_INPUT.md` 继续保留为输入材料，但在本 change 期间，真正用于实施和评审的 canonical target contract 以：

- 本 change 的 `proposal/design/tasks`
- 本 change 的 delta specs
- 以及后续同步进 `openspec/specs/` 的 current contract

为准。

这样做的理由是输入材料里曾出现会直接误导实现的错误表述：把 `vm`/`m` 位写成 `v0-mask` 语义。先把该类歧义收敛到 active artifacts，才能避免实现阶段把错误语义固化。

**Alternatives considered:**

- 直接以根目录 Markdown 为唯一规范继续实现：Rejected，因为它当前不是 current contract，且已经暴露出与仓库 current context 的冲突。

### 2. Use the shared active baseline before implementation

本 change 不再自己决定 baseline/oracle/shared semantics，而是直接采用 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 中已冻结的共享前提。当前至少包括：

- `.version 7.8` / `sm_90` project-wide PTX baseline
- repository-managed reference-model oracle
- packed `f16x2` / `bf16x2` 的 32-bit container 语义
- MMA 首发只承诺 `native-mma-sync` 子集的边界

理由：

- 如果 change 1 先按某个 baseline 落地，而 change 2 再要求继续抬升，就会导致 current contract、README、回归默认值和 driver 心智模型被二次改写；
- custom 指令支持最终需要一个单一、可维护的 project-wide baseline，而不是“一部分 `.version 7.0` / 一部分 `.version 8.0`”；
- 用户已明确接受先用 active 文档冻结共享前提、再由两个 active changes 实施的顺序。

**Alternatives considered:**

- 由本 change 单独先定 baseline：Rejected，因为 MMA change 很可能继续改变所需目标，导致项目级 contract 重复收敛。
- 允许不同 active changes 各自用不同 `.version/.target`：Rejected，因为这会拆裂 current contract 与默认验证口径。

### 3. Decoder becomes a three-way front-end

前端解码将从当前“Spike-backed pattern + scalar fallback”扩展为三路结构：

1. repo-local custom decode
2. Spike-backed Ventus/RVV pattern match
3. scalar fallback (`decode_scalar`)

repo-local custom decode 负责本 change 新增的 custom opcode 家族，例如：

- `0x42` shuffle
- `0x0B` vcvt
- `0x2B` SFU
- `0x5B` packed arithmetic

设计上，repo-local custom decode 应优先于 Spike pattern，以避免未来 opcode overlap 时仍然被旧的 pattern 入口先吞掉。

本 change 负责建立通用 custom decode framework 与 non-MMA 家族的正式 decode；MMA 专属的 opcode `0x0A` 语义、matrix、metadata 与正式 decode ownership 归 `support-custom-mma`。

**Alternatives considered:**

- 继续强制所有 custom 指令进入 Spike `DECLARE_INSN` 后再接入：Rejected，因为这会把项目节奏绑定到外部上游。
- 把所有元数据编码进 `di.name`：Rejected，因为会让 emitter、测试和覆盖统计都变得脆弱。

### 4. Extend decode IR with explicit custom metadata needed by non-MMA families, while keeping the structure extensible for MMA

`DecodedInst` 需要从“仅通用寄存器/立即数”扩展到能表达 custom 元数据的最小 IR。本 change 至少需要的字段包括：

- custom opcode family / sub-op kind
- packed / convert / SFU dtype

设计原则是：这些信息应该是结构化字段，而不是塞进字符串名字或重复拆 bit。字段布局同时应预留后续 MMA change 可继续扩展 `shape/layout/type` 的空间，但这些 MMA 专属字段不由本 change 负责定稿。

这样 non-MMA change 与后续 MMA change 才能共享同一个 decode 基座。

此外，non-MMA custom 指令若保留 OP-V 同构的 `vm`/`m` 位，该位在当前 Ventus ISA 语义下也只应被视为编码位，而不是行为开关；decoder/lowering 不得据此读取 `v0` 或引入独立 mask 路径。

同样需要遵守 shared baseline 中已冻结的 packed canonical contract：packed `f16x2` / `bf16x2` 指令按 32-bit container 解释，`vl` 计 container 数，而不是把其重新解释为普通 `vsew=16` 逐元素语义。

**Alternatives considered:**

- 先完全按 non-MMA 最小需求硬编码，后续 MMA 再推翻 IR：Rejected，因为会导致两次大改 IR，并让本 change 的产物很快过时。

### 5. This change covers every non-MMA family, with native-first lowering and explicit synthesis when native PTX is missing

本 change 的范围固定为“除 MMA 之外的全部 custom 指令”，不再拆更小批次。具体 lowering 策略如下：

- `shuffle`: 直接映射到 `shfl.sync.idx/up/down/bfly.b32`
- `vcvt`: 直接映射到 PTX `cvt` / packed convert
- packed `f16x2` / `bf16x2` arithmetic:
  - 优先用原生 `add/mul/fma.*x2`
  - 不再为低 SM 保留兼容实现
- `fp32` SFU:
  - 对 `ex2/lg2/rcp/sqrt/rsqrt/sin/cos` 用原生 PTX
  - `tanh/gelu/silu` 用显式组合序列展开
- packed `f16x2` / `bf16x2` SFU:
  - 原生 PTX 存在时直接映射
  - 原生 PTX 缺失时走 unpack -> per-half compute -> repack 的显式 lowering

这里有一个关键判断：即使本 change 覆盖“其它所有指令”，它也仍然是可做的，因为缺失原生 PTX 的 packed SFU 子操作可以用显式 lowering 补齐；但这要求一开始就接受“native-first, synthesis-second”的实现模式。

**Alternatives considered:**

- 把 packed `bf16x2` SFU 再拆到额外 Phase 1.5：Rejected，因为用户已经明确希望第一阶段覆盖 MMA 之外的所有指令。
- 对缺失原生 PTX 的子操作直接报 unsupported：Rejected，因为这会让第一阶段名义上“覆盖全部 non-MMA”，实际上却留下结构性空洞。

## Risks / Trade-offs

- **Shared baseline 漂移风险**：如果 `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` 与两个 active changes 不同步更新，就可能再次出现项目级口径分裂。
- **规范漂移风险**：如果 `doc/CUSTOM_INSTRUCTION_INPUT.md` 后续继续独立演化，而未同步回 OpenSpec/current docs，会再次出现“输入材料”和“正式 contract”分裂。
- **non-MMA change 复杂度低估风险**：虽然 MMA 被拆走了，但本 change 仍然需要统一 IR 改造、PTX baseline 抬升、packed synth lowering、microtest 补齐，不是简单的小改。
- **packed SFU 数值风险**：对原生 PTX 不存在的 packed SFU 子操作，显式 unpack/repack lowering 会带来额外数值细节与实现复杂度，需要 microtest 明确暴露。
- **后续 change 耦合风险**：MMA change 仍会依赖本 change 的 decode/IR/baseline 决策；如果这里把边界定义得过死，后续 MMA change 可能被迫再次改动 shared foundations。
