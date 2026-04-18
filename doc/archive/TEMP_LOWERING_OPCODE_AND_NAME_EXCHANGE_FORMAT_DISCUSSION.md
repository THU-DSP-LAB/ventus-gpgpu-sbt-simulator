# lowering 路径中 opcode / name 交换格式讨论纪要（临时）

本文记录一轮围绕 “`decode -> lowering -> PTX emit` 之间应以什么格式交换指令语义” 的方案讨论。本文目标不是立刻冻结实现，而是把背景、源码现状、当前判断、保守可落地方案与未来演进方向整理清楚，供后续真正重构 lowering 前参考。

临时性质说明：
- 本文是阶段性设计讨论纪要，不是当前 contract，不构成实现承诺。
- 本文不作为 `README.md`、`doc/IMPLEMENTATION_CODEMAP.md` 或 `openspec/specs/` 的入口文档引用。
- 当前优先级是把问题定义清楚、识别错误方向，并整理出一条可实施且不自相矛盾的演进路线，而不是立即完成全仓重构。

## 1. 背景与需求

当前仓库主线为：
- `ELF(.riscv) -> decode -> CFG verify -> PTX emit`

在这条路径里，`decode` 与 `ptx emit` 之间长期存在一个架构问题：
- 大量 lowering 决策仍通过指令名字符串（`DecodedInst.name`）完成；
- 这使 `name` 同时承担了人类可读 mnemonic、调试输出、以及内部 lowering 语义分派三种职责；
- 一旦 mnemonic 改名、补别名、或某个 family 想脱离名字前后缀推断，decode 与 lowering 之间就容易出现事实源分裂。

本轮讨论要回答的问题是：
- 当前“以指令名字符串为主交换格式”的做法是否合理；
- 如果不合理，当前最稳妥的改进方案是什么；
- 若未来进一步演进到 code-generated `Opcode enum`，lowering 结构应如何设计；
- 在尚未引入统一 `Opcode enum` 之前，`name` 应保留到什么程度。

## 2. 当前源码现状

### 2.1 `DecodedInst` 已经不是纯字符串载体

当前 `DecodedInst` 已经包含多类结构化字段，而不是只存一个 mnemonic：
- `inst_id`
- `operand_form`
- `uniform_transfer_kind`
- `scalar_exec_kind`
- `custom`
- `mma`

对应定义见：
- `sbt/riscv_decode.hpp`

这意味着仓库现状并不是“完全没有结构化基础”，而是：
- decode 端已经开始抽取语义；
- 但 lowering 端尚未把这些语义字段真正扶正为主事实源。

### 2.2 `InstMetadata` 已经成为一部分共享语义来源

当前 `instruction_metadata.cpp` 已为大量 Spike-backed / scalar 指令维护了显式 metadata：
- `operand_form`
- `imm_kind`
- `uniform_transfer_kind`
- `scalar_exec_kind`

decode 时会把这些 metadata 填入 `DecodedInst`。

因此对普通非-custom 指令来说，仓库当前已经具备：
- “从文本 mnemonic 走向结构化 contract” 的基础设施；
- 但该 contract 目前尚未在 PTX emitter 主路径中被完整消费。

### 2.3 `inst_id` 当前并不是真正的 opcode

当前 `InstMetadata.id` / `DecodedInst.inst_id` 的生成方式是：
- `fnv1a(name)`

也就是说：
- 它本质上仍然是 `hash(name)`；
- 它不是一个独立于 `name` 的语义 identity；
- 它并不能替代真正的 `Opcode enum`。

因此，若简单把：
- `if (di.name == "add")`

替换成：
- `if (di.inst_id == make_inst_id("add"))`

本质收益有限，只是把字符串比较换成了整数比较，并没有解决：
- 重命名脆弱；
- lowering 事实源分裂；
- 无法做编译期穷尽检查；
- 语义分类仍依赖名字本身。

### 2.4 `ptx_emit.cpp` 仍然是大规模字符串分派

当前 `sbt/ptx_emit.cpp` 文件规模已超过 3600 行，主 lowering 逻辑中存在大量：
- `if (di.name == "...")`
- `if (is_scalar_branch(di.name))`
- `if (di.name.find(...))`

这说明：
- `name` 目前仍是 PTX lowering 的主交换格式；
- 当前 emitter 结构上更接近“巨型字符串分派器”，而不是“结构化 descriptor 驱动的 lowering”。

### 2.5 custom non-MMA 与 MMA 处于不同成熟度

当前 custom 路径内部其实已经分化出两种形态。

custom non-MMA：
- decode 已经产出 `CustomFamily / CustomSubOp / CustomDataType`；
- 但 emitter 中 4 个 family 仍都直接或间接消费 `di.name` 做 lowering 分派；
- `shuffle` / `vcvt` / packed arithmetic 当前仍直接按具体 mnemonic 分支；
- SFU 当前虽优先使用 `custom.subop`，但仍保留 `_approx_*` 名字前后缀回退与 dtype 后缀判断；
- 属于“结构化信息已经存在，但尚未彻底接管 lowering”。

MMA：
- decode 产出 `MmaInstInfo`；
- lowering 继续消费 `shape/layout/type/lowering_class/support_class`；
- 再通过 `ptx_mma` 中的 ABI / tuple / planner helper 做进一步 lowering；
- 这是当前仓库里“结构化 decode -> 结构化 lowering”的成熟正例。

### 2.6 `name` 目前仍是对外 contract 的一部分

需要明确：
- `DecodedInst.name` 目前不只是内部实现细节；
- `sbt_decode pretty`、JSON 输出、coverage 脚本、部分测试夹具都直接使用 `name`。

因此不能把 `name` 简单视为“应立即删除的遗留字段”。

更准确的说法是：
- `name` 当前应该保留为外部可见 mnemonic contract；
- 但不应该继续承担 lowering 主事实源角色。

## 3. 当前方案评估：仅以指令名字符串作为交换格式好不好

当前结论是：
- 作为 bring-up 阶段手段，这种做法是可以接受的；
- 作为当前长期主线架构，这种做法并不好。

主要问题包括：

### 3.1 重命名脆弱

当字符串既代表：
- pretty 输出名称；
- JSON 名称；
- lowering 分派键；

则任何改名都会产生额外风险：
- decode 看起来仍成功；
- 但 emit、CFG classify、verify、测试夹具可能出现遗漏或错配。

### 3.2 事实源分裂

当前同一条指令的“语义来源”可能同时来自：
- `name`
- `inst_id`
- `operand_form`
- `scalar_exec_kind`
- `uniform_transfer_kind`
- `custom.*`
- `mma.*`

如果各模块各取一部分，又没有明确规定谁是主事实源，维护成本会持续升高。

### 3.3 编译器无法帮助做穷尽检查

字符串链天然缺乏：
- enum exhaustiveness；
- switch case completeness；
- codegen 对齐检查；

因此新增指令时，更容易出现“某处忘记补处理逻辑，但编译器不会报错”的问题。

### 3.4 不利于抽象同类语义

很多当前靠名字判别的逻辑，本质上是同一语义类：
- branch condition
- load/store width + sign
- scalar FP op class
- custom family/subop/dtype

如果继续依赖 `name`，这些同类语义就会被 mnemonic 文本切碎。

## 4. 当前讨论中已排除的错误方向

### 4.1 方向 A：继续把 `name` 当主交换格式，只做局部修补

即：
- 保持 emitter 主体仍按名字分派；
- 遇到明显问题再补几个 helper 或 if-else。

问题：
- 不能收敛事实源；
- 只能继续放大 `ptx_emit.cpp`；
- 结构上仍是“字符串 lowering”。

当前结论：
- 不建议继续沿此路线累积新逻辑。

### 4.2 方向 B：不引入真正 opcode，只用 `inst_id = hash(name)` 取代 `name`

这个方向的诱惑是：
- 改动小；
- 看起来“像用了 id 而不是字符串”。

但问题是：
- `inst_id` 当前只是 `hash(name)`；
- 它没有提供新的语义层；
- 仍没有摆脱名字依赖。

当前结论：
- 该方向没有足够架构意义；
- 不应被包装成“已经完成去字符串化”。

### 4.3 方向 C：立即上完整全仓 `Opcode enum + 全量 descriptor + 全模块同步改造`

这个方向理论上最彻底，但当前阶段问题在于：
- 一次性改造面过大；
- 会同时触及 decode、CFG、verify、ptx emit、CLI、coverage、测试夹具；
- 在当前仓库里容易形成高 churn、长时间半成品、双事实源并存。

当前结论：
- 可作为未来演进方向思考；
- 不适合作为最近一步的实现方案。

## 5. 当前阶段更稳妥的现有方案

本轮讨论后，当前更稳妥的方案是：

### 5.1 保留 `name`，但降级其职责

`name` 应继续承担：
- pretty print
- JSON / 诊断
- coverage / mnemonic 对照
- 少量局部 fallback 分类

但不再承担：
- lowering 主交换格式
- 大面积 PTX emit 分派主键

### 5.2 优先用现有结构化字段接管 lowering 主路径

在不引入统一 `Opcode enum` 的前提下，当前最现实的做法不是去扶正 `inst_id`，而是直接使用已有语义字段：

普通指令：
- `operand_form`
- `imm_kind`
- `scalar_exec_kind`
- `uniform_transfer_kind`
- 后续按需补充的小型 lowering descriptor

custom non-MMA：
- `custom.family`
- `custom.subop`
- `custom.dtype`

MMA：
- `mma.*`
- `ptx_mma` 中的 planner / ABI descriptor

也就是说，当前阶段的重点不是“找一个新的主键”，而是：
- 把 lowering 从 identity-driven 改为 semantic-descriptor-driven。

但需要明确一个当前边界：
- 对普通非-custom 指令来说，现有 `operand_form / imm_kind / scalar_exec_kind / uniform_transfer_kind` 仍不足以单独决定具体 lowering；
- 这些字段当前更适合作为门禁、分类与共享 contract，而不是直接替代所有 opcode 级 emit 分派；
- 若后续继续推进普通指令的去字符串化，应按实际需要补充更贴近 lowering 语义的 descriptor，而不是预设一套一次性冻结的大而全字段集。

当前已知常见例子包括：
- branch condition kind（如 `eq/ne/lt/ge` 与 signed/unsigned）
- load/store descriptor（宽度、符号扩展、raw-bit / integer 语义）
- scalar ALU / scalar FP op class
- vector compare / vector FP / mask boolean op class

这些例子是当前讨论下的建议方向，不应被写成未来实现必须逐条照抄的硬性字段清单；实现时只要新增字段能清晰承载 lowering 语义、避免继续依赖名字推断即可。

### 5.3 优先重构 `ptx_emit.cpp`，而不是全仓同步重写

当前最有价值的目标是：
- 收缩 `ptx_emit.cpp` 中的大规模字符串分派；
- 让真正已有 payload 的部分先脱离 `name`。

这比全仓同步推倒重来更可实施，也更符合当前代码实际形态。

### 5.4 custom non-MMA 应作为近期优先收口点

原因：
- decode 已经提供 `family/subop/dtype`；
- emitter 中仍存在明显名字回退；
- 这块收益高、风险低、依赖少。

当前 residual 名字依赖可明确列为：
- `shuffle`：按 `shuffle_idx/up/down/bfly` 名字分派；
- `vcvt`：按 `vcvt_*` 名字分派；
- packed arithmetic：按 `vadd/vmul/vfma + f16x2/bf16x2` 名字分派；
- SFU：按 `*_approx_f32/f16x2/bf16x2` 名字分派 dtype，并在 payload 缺失时按名字前缀回推 `subop`。

当前建议：
- custom non-MMA lowering 应改为仅消费结构化 payload；
- `name` 保留给 pretty print / JSON / 诊断，不再作为 lowering 主判据；
- 删除 `_approx_*` 等按名字前后缀反推 `subop` 的回退逻辑；
- 对已经迁移的 custom non-MMA family，不再允许 emitter 继续解析 mnemonic 前后缀决定 lowering 分支。

进一步地，当前更合适的 contract 是：
- 对 custom non-MMA，`custom.family / custom.subop / custom.dtype` 与操作数字段是 lowering 主事实源；
- 若 `DecodedInst.name` 与这些结构化字段表达的语义不一致，应显式报错；
- 不应在冲突时静默选择“相信名字”或“相信 payload”中的任一侧继续 lowering。

这样做的目的不是增加防御性分支，而是明确暴露 decode / emitter 事实源错位，使问题在开发期直接失败，而不是继续把 `name` 留在 correctness path 上。

### 5.5 MMA 现有模式应被视为正例模板

MMA 当前已体现出较清晰的分层：
- decode：产出 `MmaInstInfo`
- lowering：检查 `support_class / lowering_class`
- planner：用 ABI / tuple / logical tile mapping helper

当前判断：
- 这条路线不应被打乱；
- 相反，后续非-MMA lowering 的演进应尽量借鉴这种“先结构化、再 lowering”的模式。

## 6. 当前方案下 `name` 还会在什么地方保留

需要明确：
- 当前不引入统一 `Opcode enum`，并不意味着可以完全禁用 `name`。

更现实的目标是：
- `name` 从“全局主事实源”降为“局部 residual fallback”。

在当前阶段，`name` 仍可合理保留在以下场景：
- pretty print / JSON / coverage；
- builtin callee symbol 的外部 ABI 名；
- 少量尚未抽出 descriptor 的局部穷尽分支；
- 诊断信息与报错文本。

但这类保留应满足两个条件：
- 范围小；
- 语义上明确是过渡性局部分类，而不是 lowering 主干。

## 7. 未来演进：若后续引入 code-generated `Opcode enum`

### 7.1 `Opcode enum` 的作用

若未来要进一步演进到 code-generated `Opcode enum`，其合理职责应是：
- 提供稳定 identity；
- 与 metadata 表、mnemonic、decoder 映射一起由同一份定义生成；
- 替代当前 `hash(name)` 这种派生 id。

它不应被设计成：
- 只是 `name` 的另一层手写镜像；
- 或只是把字符串 if-else 改写成更长的 enum switch。

### 7.2 `Opcode enum` 引入后 lowering 应如何组织

未来较理想的 lowering 结构应是：

- `Opcode` 提供 identity；
- generated metadata / descriptor 提供语义；
- emitter 顶层按 lowering family 分派；
- 局部再按 semantic descriptor 或少量 opcode exhaustiveness 处理。

不建议退化成：
- 一个几百 case 的扁平 `switch(opcode)` 覆盖整个 emitter。

更合理的形态是：

- 顶层：
  - `emit_control`
  - `emit_scalar`
  - `emit_vector`
  - `emit_custom`
  - `emit_mma`

- 家族内部：
  - 尽量按 descriptor 分类，如 branch cond、mem desc、alu/fp op class；
  - 只有在小范围、确实离散的子类里再 `switch(opcode)`。

### 7.3 `Opcode enum` 进入后，custom / MMA 仍不应回退到“只看 opcode”

即使未来有统一 `Opcode enum`：
- custom non-MMA 仍更适合按 `family/subop/dtype` lowering；
- MMA 仍更适合按 `mma.* + abi desc` lowering。

原因：
- 对这些家族来说，payload 本身比单个 opcode identity 更贴近真正的 lowering 语义。

因此未来 `Opcode enum` 的主要价值是：
- 统一 identity；
- 支持生成 metadata；
- 提升测试与编译期穷尽性；

而不是逼迫所有 lowering 全都重新退化成单点 opcode switch。

## 8. 当前方案与未来演进之间的衔接关系

本轮讨论后的关键判断是：

- 当前阶段不应急于扶正 `inst_id = hash(name)`；
- 当前阶段优先做“descriptor 驱动”的局部收口；
- 未来若要引入真正 code-generated `Opcode enum`，它应建立在“语义字段已经成为主事实源”的基础上，而不是反过来重新把 lowering 压回 identity-first。

也就是说，当前方案并不是未来方案的对立面，而是未来方案的更保守前置阶段：

1. 先让 `name` 退出 lowering 主干；
2. 再让 payload / metadata 成为真正主事实源；
3. 最后若有需要，再把 `hash(name)` 升级成真正的 generated `Opcode enum`。

这样的顺序更稳，也更不容易制造双事实源。

## 9. 当前暂定结论

当前阶段的暂定结论如下：

1. 继续以指令名字符串作为 lowering 主交换格式，并不好，不宜继续扩大使用面。
2. 但当前阶段也不应把 `inst_id = hash(name)` 误当成真正 opcode 主键。
3. 当前更合理的做法是：
   - 保留 `name` 作为外部 mnemonic contract；
   - 在 lowering 主路径中优先改用已有 payload / metadata；
   - 把 `name` 收缩为小范围局部 fallback。
4. custom non-MMA 是当前最适合先收口的区域，目标 contract 应是“payload authoritative，`name` 仅用于外部输出；二者冲突时显式报错”。
5. MMA 现有结构化 lowering 模式应被保留，并作为后续演进模板。
6. 对普通非-custom 指令，现有共享 metadata 仍主要承担门禁与共享 contract 角色；若继续推进去字符串化，应按实际需要补充合理 descriptor，而不是预先冻结一套强制字段清单。
7. 若未来引入 code-generated `Opcode enum`，应让它承担稳定 identity，而 lowering 仍应主要由 metadata / semantic descriptor 驱动，而不是重新退回到大号 opcode switch。

## 10. 本文用途

本文的用途是：
- 作为当前一轮架构讨论的临时纪要；
- 帮助后续真正修改 lowering 时避免重复踩入错误方向；
- 为未来是否引入 generated `Opcode enum` 提供一个不混淆“identity”与“semantic descriptor”的讨论基线。

待后续真正开始实现时，应根据实际落地范围决定：
- 是将本文升级沉淀为正式设计文档；
- 还是仅保留为一次临时讨论记录并继续归档。
