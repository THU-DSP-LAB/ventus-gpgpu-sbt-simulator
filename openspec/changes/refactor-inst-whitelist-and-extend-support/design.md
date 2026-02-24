# Design: Single-source whitelist + staged instruction support expansion

## 1. Glossary
- **Whitelist / want**：Spike `encoding.h` 里 `DECLARE_INSN(<id>, ...)` 的 `<id>` 集合（下划线命名，如 `vadd_vv`）。
- **Pattern**：由 `<id, match, mask>` 组成的匹配表项，用于 Ventus 扩展/向量类指令的解码命中。
- **Supported**（本变更语境）：指“可通过 decode + CFG verify + PTX emit 并可 `ptxas` 编译”；不以端到端数值正确性作为本变更验收门槛（仍由现有回归/测例覆盖）。

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

## 4. Extending instruction support (no new diagnostics)
本变更对“完善指令支持”的策略是：按指令族系统性补齐 emitter 与必要的 decode 分类，而不是逐条 case-by-case 临时加补丁。

### 4.1 Stage A: Complete scalar RV32I/M subset in PTX emitter
目标：对 `sbt/riscv_decode.cpp` 已能标识的 RV32 标量指令，PTX emitter 支持应达到“常用子集完整”，至少覆盖：
- RV32I ALU：`and/or/xor/andi/ori`、`sll/srl/sra`、`srli/srai`（当前已支持 `slli`）
- RV32M：`div/divu/rem/remu`、`mulh/mulhsu/mulhu`（当前仅 `mul`）
- Load/Store：补齐 `lb/lh/lhu/sh`（当前仅实现 `lw/lbu/sw/sb`）

约束：
- 仍遵循现有 “warp-uniform 标量语义：WarpCtx + leader-only 可选” 的实现方式。
- 不改变现有 CFG 约束（如 non-ret `jalr` 仍不支持）。

### 4.2 Stage B: Complete basic Ventus vector memory ops
目标：补齐 `VentusInst_basic.txt` 中基础向量访存（12-bit offset forms）在 decode 分类与 emitter lowering 的支持：
- `vlb12.v / vlbu12.v`
- `vlh12.v / vlhu12.v`
- `vsb12.v / vsh12.v / vsw12.v`

实现原则：
- 复用现有“数值地址空间区间分流”的地址映射模型；
- 对 u16/u8 扩展或符号扩展行为与 Ventus 指令表一致；
- 不引入新诊断工具。

### 4.3 Stage C: Expand common RVV-like integer ops (as-needed)
目标：按现有回归/测例需求补齐常见的向量逻辑/算术（示例）：
- `vand.vx / vor.vx / vxor.vv / vxor.vx`、`vadd/vsub` 的缺失变体
- `vsll.vv/vsll.vx`、`vsrl.vv/vsrl.vx`、`vsra.vv/vsra.vx`
- `vmul{,h}.*` 的缺失变体、`vrem*`/`vdiv*` 的缺失变体

该阶段不承诺一次性覆盖 `VentusInst_basic.txt` 中所有 RVV 指令；以“先覆盖实际输入出现的常用集合”为原则推进。

## 5. Acceptance criteria
- Whitelist 一致性：
  - 仓库中不存在重复的硬编码 want 列表（`tools/sbt_ptx.cpp` / `tools/sbt_decode.cpp` / `tools/gen_spike_encoding_subset.cpp`）。
  - 更新 `data/spike_want.txt` 后，三处工具加载到的 pattern 子集一致。
- 指令支持（compile-first 维度）：
  - `tools/rodinia_ptx_smoke.sh` 覆盖的 kernel 能继续通过 `sbt_ptx --require-known` + `ptxas` 编译。
  - 新增补齐的 scalar/vector 指令族有对应的最小覆盖（可由现有 Rodinia/PoCL 测例触发，不要求新增诊断工具）。

