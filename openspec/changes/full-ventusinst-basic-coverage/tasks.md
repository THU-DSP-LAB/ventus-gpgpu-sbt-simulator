## Implementation Tasks

## 0. Scope + exceptions
- [ ] 0.1 新增并维护“指令支持例外列表”（初始仅 `jalr` 非 `ret` 形态）；新增例外前先告知确认

## 1. Coverage accounting (mnemonic-level, no missing-name list)
- [ ] 1.1 将 `VentusInst_basic.txt` 解析为目标 mnemonic 集合（`.`→`_`）并纳入测试 gate（只输出统计）
- [ ] 1.2 将 micro-test 的 `sbt_decode --json` 结果汇总为“被触达 mnemonic 集合”，并与目标集合求交输出覆盖率
- [ ] 1.3 将 gate 提升到“全覆盖（除例外）”并纳入 CI/回归脚本

## 2. Expand whitelist/patterns toward full table
- [ ] 2.1 扩展 `data/spike_want.txt`，覆盖 `VentusInst_basic.txt` 中所有需要 Spike pattern 的指令（custom/RVV/Ventus 扩展）
- [ ] 2.2 确保 `sbt_ptx/sbt_decode/gen_spike_encoding_subset` 在更新 want 后行为一致

## 3. Decode classification completeness
- [ ] 3.1 补齐 `sbt/riscv_decode.cpp` 的分类/operand 规则，使 `VentusInst_basic.txt` 中所有指令都能被正确解码为寄存器/立即数形态
- [ ] 3.2 为 regext/regexti 前缀 bundling 相关边界情况增加最小回归（不改变现有语义）

## 4. PTX lowering completeness (compile-supported)
- [ ] 4.1 逐指令族补齐 `sbt/ptx_emit.cpp` lowering，使 `--require-known` 下不因 `unsupported.inst` 失败（除例外）
- [ ] 4.2 对“缺少直接等价 PTX 指令”的情况：优先转化为指令序列；无法处理的进入例外流程（需确认）
- [ ] 4.3 对浮点相关指令：允许语义近似实现（不要求 bit-accurate），但需被微测例覆盖与容差对比

## 5. Micro-tests (semantic-supported via Spike oracle)
- [ ] 5.1 按指令族扩展 `testcases/` 下的 OpenCL 微测例，使所有 mnemonic 至少被一条 kernel 覆盖
- [ ] 5.2 扩展对照运行器：支持按 kernel 列表运行、输出对照结果、并支持浮点容差比较
- [ ] 5.3 将微测例与对照运行加入回归入口（默认 1 block×1 warp 的最小 NDRange）

## 6. Regression guard
- [ ] 6.1 `tools/rodinia_ptx_smoke.sh` 保持通过（compile-first）
- [ ] 6.2 端到端：`VENTUS_BACKEND=spike` 与 `VENTUS_BACKEND=ptx` 的 PoCL example/Rodinia 既有通过集不回退

