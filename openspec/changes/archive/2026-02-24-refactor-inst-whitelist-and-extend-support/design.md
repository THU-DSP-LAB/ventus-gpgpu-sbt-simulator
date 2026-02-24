# Design: Single-source whitelist + staged instruction support expansion

## 1. Glossary
- **Whitelist / want**：Spike `encoding.h` 里 `DECLARE_INSN(<id>, ...)` 的 `<id>` 集合（下划线命名，如 `vadd_vv`）。
- **Pattern**：由 `<id, match, mask>` 组成的匹配表项，用于 Ventus 扩展/向量类指令的解码命中。
- **Compile-supported**：指“可通过 decode + CFG verify + PTX emit 并可 `ptxas` 编译”。
- **Semantic-supported**：指“在可运行的端到端链路下，与 Spike（Ventus PoCL device）结果一致（浮点允许近似误差）”。

## 2. Single source of truth: `data/spike_want.txt`
新增文件：
- `data/spike_want.txt`
  - 格式：一行一个 insn id（与 Spike `DECLARE_INSN` 一致，使用下划线）
  - 允许空行与 `#` 注释
  - 该文件是唯一 whitelist 来源；任何 C++ 源码中不得再出现硬编码 `want = { ... }` 的指令 ID 列表。

约束：
- `tools/sbt_ptx.cpp`、`tools/sbt_decode.cpp`、`tools/gen_spike_encoding_subset.cpp` 必须共用同一份 want 文件。
- want 文件更新是“扩展 Ventus pattern 支持”的唯一入口（先让 decode 识别，再决定是否补 emit）。

## 3. Pattern loading strategy

### 3.1 Runtime filtering (baseline)
保持现有行为：工具仍然解析 `--encoding-h` 指定的 Spike `encoding.h`（默认 `ventus-env/spike/riscv/encoding.h`）。
变更点：解析出的 `declared_insns` 必须用 `data/spike_want.txt` 过滤，生成最终 `std::vector<Pattern>`。

优点：
- 变更小、风险低；
- 立即消除三处 want 列表不一致问题；
- 不引入新的诊断输出需求。

### 3.2 (Optional) Generated subset header (stability improvement)
将 `tools/gen_spike_encoding_subset` 改为读取 `data/spike_want.txt`，生成：
- `sbt/generated/spike_encoding_subset.hpp`（只包含 want 子集的 `{name, match, mask}`）

后续可选策略：
- `sbt_ptx/sbt_decode` 默认优先使用生成头文件（减少运行时对 `encoding.h` 文本解析的依赖），仅在显式开关下回退到 runtime parse。

本变更不强制落地该策略；若落地，需保证不引入额外“缺失指令名输出”的新工具行为。

## 4. Instruction coverage target (`VentusInst_basic.txt`)
目标集合以仓库内 `VentusInst_basic.txt` 为准：原则上该表中的每条指令都应达到至少 **Compile-supported**，并在具备可测路径后逐步提升到 **Semantic-supported**。

### 4.1 Explicit exceptions (allowed fail-fast)
以下情况允许保持“遇到即直接报错/fail-fast”的策略（需要在实现与文档中显式标注）：
- 非 `ret` 形态的 `jalr`（间接跳转/间接调用/跳转表）
- 其它破坏当前结构化翻译前提的控制流形态（如果未来出现）
- `regexti`：可作为“暂不要求支持”的例外（但 `regext` 前缀已在 decode bundling 中使用，属于必须支持的解码特性）

说明：
- `regext/regexti` 在当前实现中是“前缀扩展”而非真正需要 lowering 的语义指令：当启用 `bundle_regext` 时，它们应被 bundle 到后继指令并在解码阶段生效；不应进入后端作为独立指令去执行。

## 5. Extending instruction support (no new diagnostics)
策略：按指令族系统性补齐 emitter 与必要的 decode 分类，并通过“对照 Spike 的 OpenCL 输出 buffer”建立语义回归；避免引入新的“扫描/报表/缺失清单输出”类工具。

### 5.1 Stage A: Complete scalar RV32I/M subset in PTX emitter
目标：对 `sbt/riscv_decode.cpp` 已能标识的 RV32 标量指令，PTX emitter 支持应达到“常用子集完整”，至少覆盖：
- RV32I ALU：`and/or/xor/andi/ori`、`sll/srl/sra`、`srli/srai`（当前已支持 `slli`）
- RV32M：`div/divu/rem/remu`、`mulh/mulhsu/mulhu`（当前仅 `mul`）
- Load/Store：补齐 `lb/lh/lhu/sh`（当前仅实现 `lw/lbu/sw/sb`）

约束：
- 仍遵循现有 “warp-uniform 标量语义：WarpCtx + leader-only 可选” 的实现方式。
- 不改变现有 CFG 约束（如 non-ret `jalr` 仍不支持）。

### 5.2 Stage B: Complete basic Ventus vector memory ops
目标：补齐 `VentusInst_basic.txt` 中基础向量访存（12-bit offset forms）在 decode 分类与 emitter lowering 的支持：
- `vlb12.v / vlbu12.v`
- `vlh12.v / vlhu12.v`
- `vsb12.v / vsh12.v / vsw12.v`

实现原则：
- 复用现有“数值地址空间区间分流”的地址映射模型；
- 对 u16/u8 扩展或符号扩展行为与 Ventus 指令表一致；
- 不引入新诊断工具。

### 5.3 Stage C: Expand RVV/Ventus integer + permute ops (toward full table)
目标：按“指令族”推进，最终覆盖 `VentusInst_basic.txt` 的全部整数与位运算相关指令。实现上优先：
- `vand.vx / vor.vx / vxor.vv / vxor.vx`、`vadd/vsub` 的缺失变体
- `vsll.vv/vsll.vx`、`vsrl.vv/vsrl.vx`、`vsra.vv/vsra.vx`
- `vmul{,h}.*` 的缺失变体、`vrem*`/`vdiv*` 的缺失变体

在进入“全表覆盖”的后半段时，难点主要集中在：
- lane-crossing/permute/reduction（需要 shfl/shared-memory 或显式循环）
- mask/compare 产生的 predicate 语义与写回规则统一化

## 6. Validation strategy: Spike oracle via OpenCL buffers
不新增“ELF 扫描/缺失清单报表”工具的前提下，验证依靠可运行的端到端对照：

### 6.1 Principle
- 同一个 device 程序（Ventus ELF）应当能在两条路径上运行：
  - Spike 路径：Ventus PoCL device（spike 作为功能仿真后端）
  - PTX 路径：当前工程的 PTX 设备（SBT + PTX JIT）
- 主机端用 OpenCL 创建输入 buffer A / 输出 buffer B，device 程序将结果写入 B；主机端 `clEnqueueReadBuffer` 读回，对比结果。

### 6.2 Micro-tests (per-instruction or per-family)
- 为 `VentusInst_basic.txt` 覆盖不足的问题，引入“指令微测例”作为回归入口：
  - 每个微测例以固定维度（例如 1 block × 1 warp）运行；
  - 输入从 buffer A 读取，输出写入 buffer B（保证对两条路径一致）；
  - 整数/位运算：要求逐元素完全一致；
  - 浮点：允许近似一致（绝对/相对误差阈值由测试定义）。

### 6.3 Coverage gate (without naming missing instructions)
- 在测试层面引入“覆盖门槛”，例如：
  - 断言：`VentusInst_basic.txt` 的目标集合都至少被某个微测例触达并成功完成对照；
  - 失败时不要求打印“缺失指令名列表”，只需给出测试失败与统计信息（例如通过/总数）。

## 7. Acceptance criteria
- Whitelist 一致性：
  - 仓库中不存在重复的硬编码 want 列表（`tools/sbt_ptx.cpp` / `tools/sbt_decode.cpp` / `tools/gen_spike_encoding_subset.cpp`）。
  - 更新 `data/spike_want.txt` 后，三处工具加载到的 pattern 子集一致。
- 指令支持（Compile-supported 维度）：
  - `tools/rodinia_ptx_smoke.sh` 覆盖的 kernel 能继续通过 `sbt_ptx --require-known` + `ptxas` 编译。
  - 补齐的指令族在 `--require-known` 下不会触发 `unsupported.inst`（以目标集合为准）。
- 指令语义（Semantic-supported 维度，逐步推进）：
  - 微测例对照在 Spike 与 PTX 两条路径上的输出一致（浮点按阈值允许近似）。
