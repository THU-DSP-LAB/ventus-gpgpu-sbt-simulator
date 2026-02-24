## 0. Whitelist single source
- [x] 0.1 新增 `data/spike_want.txt`，并迁移现有 bring-up whitelist 内容进入该文件
- [x] 0.2 `tools/sbt_ptx.cpp` 从 `data/spike_want.txt` 读取 want，移除硬编码 `want = { ... }`
- [x] 0.3 `tools/sbt_decode.cpp` 从 `data/spike_want.txt` 读取 want，移除硬编码 `want = { ... }`
- [x] 0.4 `tools/gen_spike_encoding_subset.cpp` 从 `data/spike_want.txt` 读取 want，移除硬编码 `want = { ... }`
- [x] 0.5 增加一个最小一致性检查（构建期或测试期）：三处加载到的 want 集合相同（不要求打印缺失指令名清单）

## 1. Define coverage target (VentusInst_basic)
- [x] 1.1 明确“目标集合 = `VentusInst_basic.txt`”与允许的 fail-fast 例外（至少：非 `ret` 形态 `jalr`，可选：`regexti`）
- [x] 1.2 引入最小的覆盖门槛检查（测试期即可）：对目标集合只输出统计，不输出“缺失指令名清单”

## 2. Instruction support: scalar RV32I/M (PTX emitter)
- [x] 2.1 补齐 RV32I ALU：`and/or/xor/andi/ori/sll/srl/sra/srli/srai`
- [x] 2.2 补齐 RV32M：`div/divu/rem/remu/mulh/mulhsu/mulhu`
- [x] 2.3 补齐 scalar load/store：`lb/lh/lhu/sh`（保持现有地址映射模型）
- [x] 2.4 确保新增指令在 `--require-known` 下不会触发 `unsupported.inst`（以现有测例覆盖为准）

## 3. Instruction support: basic vector memory ops
- [x] 3.1 decode 分类补齐：识别 `vlb12_v/vlh12_v/vlhu12_v/vsh12_v` 的 operand/imm 规则
- [x] 3.2 PTX lowering 补齐：实现对应的 load/store（含符号/零扩展）
- [x] 3.3 保持现有 `vlw12/vsw12/vlbu12/vsb12` 行为不回退

## 4. Micro-tests: Spike oracle via OpenCL buffers
- [x] 4.1 增加一个最小 OpenCL 主机端“对照运行器”：同一份 Ventus ELF，在 Spike device 与 PTX device 上分别运行，读回输出 buffer B 并对比
- [x] 4.2 为指令族补齐“微测例”（每个测例通过 A/B buffer 进行输入输出）；浮点比较使用误差阈值
- [x] 4.3 覆盖门槛逐步提升到“目标集合全覆盖”（允许的 fail-fast 例外除外）

## 5. Regression guard (no new diagnostic tools)
- [x] 5.1 `tools/rodinia_ptx_smoke.sh` 保持通过（compile-first）
- [x] 5.2 现有端到端回归（PoCL examples + Rodinia 既有通过集）不回退
