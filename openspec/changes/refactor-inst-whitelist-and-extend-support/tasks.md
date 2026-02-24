## 0. Whitelist single source
- [ ] 0.1 新增 `data/spike_want.txt`，并迁移现有 bring-up whitelist 内容进入该文件
- [ ] 0.2 `tools/sbt_ptx.cpp` 从 `data/spike_want.txt` 读取 want，移除硬编码 `want = { ... }`
- [ ] 0.3 `tools/sbt_decode.cpp` 从 `data/spike_want.txt` 读取 want，移除硬编码 `want = { ... }`
- [ ] 0.4 `tools/gen_spike_encoding_subset.cpp` 从 `data/spike_want.txt` 读取 want，移除硬编码 `want = { ... }`
- [ ] 0.5 增加一个最小一致性检查（构建期或测试期）：三处加载到的 want 集合相同（不要求打印缺失指令名清单）

## 1. Instruction support: scalar RV32I/M (PTX emitter)
- [ ] 1.1 补齐 RV32I ALU：`and/or/xor/andi/ori/sll/srl/sra/srli/srai`
- [ ] 1.2 补齐 RV32M：`div/divu/rem/remu/mulh/mulhsu/mulhu`
- [ ] 1.3 补齐 scalar load/store：`lb/lh/lhu/sh`（保持现有地址映射模型）
- [ ] 1.4 确保新增指令在 `--require-known` 下不会触发 `unsupported.inst`（以现有测例覆盖为准）

## 2. Instruction support: basic vector memory ops
- [ ] 2.1 decode 分类补齐：识别 `vlb12_v/vlh12_v/vlhu12_v/vsh12_v` 的 operand/imm 规则
- [ ] 2.2 PTX lowering 补齐：实现对应的 load/store（含符号/零扩展）
- [ ] 2.3 保持现有 `vlw12/vsw12/vlbu12/vsb12` 行为不回退

## 3. Regression guard (no new diagnostic tools)
- [ ] 3.1 `tools/rodinia_ptx_smoke.sh` 保持通过（compile-first）
- [ ] 3.2 现有端到端回归（PoCL examples + Rodinia 既有通过集）不回退

