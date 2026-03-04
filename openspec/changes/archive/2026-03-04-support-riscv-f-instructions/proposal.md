## Why

`VentusInst_basic.txt` 作为本项目的目标指令集合（覆盖 gate 的基准）近期补充了 RV32F（单精度浮点）相关指令 mnemonic。当前实现中：

- `sbt_decode` 的 scalar decoder 主要覆盖 RV32I/M + CSR，浮点仅把少量 `fmv.*` 视作可忽略的 padding/NOP；
- `sbt_ptx` 对新增 RV32F 指令缺少 lowering，在 `--require-known` 与覆盖 gate 路径下会以 unknown/unsupported fail-fast。

为了保持“以 `VentusInst_basic.txt` 为目标集合”的一致性，并提升对真实 Ventus 编译产物（Rodinia/PoCL 示例）的可翻译范围，需要将新增 RV32F 指令纳入本项目的指令支持与回归验证。

## What Changes

- 扩展前端解码：在 `sbt/riscv_decode` 中补齐 RV32F 指令解码与 operand/imm 提取，确保 `--require-known` 下不再把这些指令判为 unknown。
- 扩展后端 PTX lowering：在 `sbt/ptx_emit` 中为 RV32F 指令补齐 lowering，目标至少达到 compile-first（生成的 PTX 可被 `ptxas` 编译）。
- 补齐语义回归：扩展 microtests 与 Spike-vs-PTX 对照覆盖这些指令的常用路径；浮点对比采用容差策略（atol/rtol），不要求 bit-accurate。
- 同步文档：在仓库文档中明确本次新增的 RV32F 支持范围与已知限制（如需保留例外，必须显式列出）。

## Capabilities

### New Capabilities

- **RV32F 指令 compile-supported**：`VentusInst_basic.txt` 中新增的 RV32F 单精度指令在 `sbt_decode --require-known` 与 `sbt_ptx --require-known` 路径下可被识别并完成 PTX 生成，且 `ptxas` 可编译。
  - 覆盖 mnemonic：
    - `fmadd.s` / `fmsub.s` / `fnmsub.s` / `fnmadd.s`
    - `fadd.s` / `fsub.s` / `fmul.s` / `fdiv.s` / `fsqrt.s`
    - `fsgnj.s` / `fsgnjn.s` / `fsgnjx.s`
    - `fmin.s` / `fmax.s`
    - `fcvt.w.s` / `fcvt.wu.s` / `fcvt.s.w` / `fcvt.s.wu`
    - `feq.s` / `flt.s` / `fle.s`
    - `fclass.s`
    - `flw` / `fsw`
    - `fmv.w.x` / `fmv.x.w`

- **RV32F 指令 semantic-validated（microtest）**：为上述指令提供可回归的 Spike-vs-PTX 语义对照（浮点允许近似），并纳入现有 microtest gate。

### Modified Capabilities

- **指令覆盖 gate 与覆盖统计**：覆盖工具以更新后的 `VentusInst_basic.txt` 为目标集合进行统计与 gate；新增 RV32F mnemonic 不再被视为“长期缺失”，而是必须被实现或显式列为例外。

## Impact

- `VentusInst_basic.txt` 目标集合扩大后，未实现前会导致覆盖 gate 降低/失败；实现完成后，SBT 流水线可处理包含 RV32F 的真实 kernel 指令流，提升对 Rodinia/PoCL 输入的兼容性。
- 浮点 lowering 可能带来与 Spike 在 NaN、舍入、溢出/异常标志等角落行为的差异；这些差异需要通过 microtest 明确暴露，并在设计阶段给出一致性策略。

