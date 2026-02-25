## Why

当前工程的 PTX 后端与解码/whitelist 机制已经能跑通 Rodinia bring-up，但距离 `VentusInst_basic.txt` 的“基础指令表全覆盖”仍有明显差距，且缺乏“逐指令/逐指令族”的微测例回归，容易演化为面向测例编程：测例覆盖不到的指令长期缺失或语义漂移。

本变更希望把“指令支持”从 bring-up 子集推进到 **全表覆盖**，并用 Spike 作为语义 oracle 的 OpenCL A/B buffer 对照测试把每条指令的语义纳入可回归范围（浮点允许近似）。

## What Changes

- 将“目标指令集合”明确为 `VentusInst_basic.txt` 中的全部指令（按 mnemonic 统计），并建立覆盖门槛与回归准入条件。
- 逐指令族补齐：扩展 decode 分类、Spike pattern whitelist（如适用）、以及 PTX lowering，使其达到“可翻译 + 可运行 + 可对照验证”。
- 引入/完善微测例集合：每条指令（或可等价合并的指令族）至少有一个 OpenCL kernel 覆盖，并通过 host 端对照运行在 `VENTUS_BACKEND=spike` 与 `VENTUS_BACKEND=ptx` 下比较输出 buffer。
- 明确并收敛“允许 fail-fast 的例外集合”，避免隐性扩大。

## Capabilities

### New Capabilities
- **Full-table instruction coverage**：对 `VentusInst_basic.txt` 的指令实现全覆盖（除显式例外）。
- **Instruction micro-test suite**：覆盖到每条指令的 Spike-vs-PTX 对照微测例回归入口。

### Modified Capabilities
- **inst-support**：从“bring-up 子集 + 少量微测”提升为“全表覆盖 + 全量微测”。

## Impact

- 输入可翻译范围显著扩大（不再只依赖 Rodinia/PoCL 经验子集）。
- 维护成本从“靠测例触发 + 临时 patch”转向“按指令族系统推进 + 可验证回归”。
- 可能的代价：
  - emitter 分支与类型/地址规则需要进一步表驱动化，否则会导致一致性维护困难；
  - 微测例数量增大，回归耗时上升（可通过分组/并行/最小 NDRange 控制）。

## Explicit Exceptions (initial)

以下例外允许继续保持“遇到即报错/fail-fast”，并需要在文档与测试中显式体现：
- `jalr` 的非 `ret` 形态（间接跳转/间接调用/跳转表）

除上述之外，如在实现过程中发现必须新增例外（例如结构化控制流前提不满足的特殊控制流指令形态），需要先在 spec 中列出并告知你确认。

