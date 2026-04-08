# 2026-04-07 Handoff：`support-custom-mma` 当前实现状态落盘

- 状态：active temporary handoff note
- 日期：2026-04-07
- 关联 change：`support-custom-mma`
- 用途：给下一位接手者提供单一入口，避免重复梳理上下文

## 1) 状态分层（必须按标签阅读）

### current（当前可执行实现真相）

以下路径在仓库内已有实现并完成基本连通：

- MMA decode/metadata：
  - `sbt/riscv_decode.hpp`
  - `sbt/riscv_decode.cpp`
  - `tools/mma_decode_test.cpp`
- MMA microtest + oracle gate：
  - `testcases/ocl_compare/custom_mma_kernels.cl`
  - `tools/custom_mma_oracle.py`
  - `tools/regress.sh`
- PTX lowering 已打通并在 full gate 通过的 family：
  - `mt_custom_mma_m16n8k16_row_col_f32_f16_f16_f32`
  - `mt_custom_mma_m16n8k8_row_col_f32_tf32_tf32_f32`
  - `mt_custom_mma_m16n16k16_row_col_f32_f16_f16_f32`

当前 full gate 统计（长期稳定）：

- `full_pass=3`
- `failures=3`
- `blocked=1`

### active（当前仍在推进）

剩余失败 family（仍在 `support-custom-mma` 当前迭代范围）：

- `mt_custom_mma_m16n8k16_row_col_f32_bf16_bf16_f32`
- `mt_custom_mma_m16n16k16_row_col_f32_bf16_bf16_f32`
- `mt_custom_mma_m16n16k8_row_col_f32_tf32_tf32_f32`

当前最小可执行技术方向：

- BF16 两条残差优先聚焦 `C/D tuple planner` 与 `D writeback`。
- TF32 `m16n16k8 split-n` 优先聚焦 `C` slice materialization，其次 `D` merge/writeback。
- 不要先并行扩散到其它 family，先收敛共享路径。

### blocked（显式阻塞，不等于全局暂停）

- `mt_custom_mma_m16n8k16_row_col_f16_f16_f16_f16_blocked`

阻塞策略：

- 仅 `fp16 -> fp16` 这一路径显式 fail-fast/block。
- 不允许把“整个 MMA 路径”一起 fail。
- 该策略来源于 Ventus LLVM + Spike 的 ABI/metadata 对齐问题，等待工具链侧修复后再回收此路径。

### historical（仅回溯背景，不作为当前实现判据）

- 早期出现过 `compile-first 全过、full 全挂` 的阶段性状态；后续迭代后已收缩为当前 `3 pass / 3 fail / 1 blocked`。
- 2026-04-06 与 2026-04-07 的两份临时问题记录保留为历史推导依据：
  - `2026-04-06-mma-abi-metadata-mismatch-temp-note.md`
  - `2026-04-07-mma-full-gate-input-carrier-risk-temp-note.md`

## 2) 关键技术结论（供接手直接使用）

- 剩余 3 个失败 family 更像 sbtsim 局部实现问题，不像 LLVM/Spike contract 未定义。
- `bf16` 残差高概率在 `sbt/ptx_mma.cpp` 的 `C/D` planner 与 `sbt/ptx_emit.cpp` 的 `D` writeback 共享路径。
- `tf32 m16n16k8 split-n` 在 direct-native `m16n8k8` 已过的前提下，更像 split-n 拼接层（`C` slice 或 `D` merge/writeback）问题。
- `fp16->fp16` 的 blocked 与上述 3 个失败 family 不应混修。

## 3) 协作执行约束（本 change 的当前共识）

- 采用多 agent，但主 agent 仅做：任务拆分、进度监控、状态同步。
- develop/review/test 为三个方向，执行顺序固定：`develop -> review -> test`。
- 不要把 develop 再细拆成多个并行子任务，优先单 develop 线完成后再转 review/test。
- 小问题可便宜行事；若涉及跨组件 contract 或会埋长期坑，必须先记录并上报再决策。

## 4) 关键命令（接手者可直接复用）

构建：

- `cmake --build build -j --target mma_ptx_emit_test custom_ptx_emit_test sbt_ptx sbt_decode`

单测：

- `./build/mma_ptx_emit_test`
- `./build/custom_ptx_emit_test`

全量语义 gate：

- `python3 tools/custom_mma_oracle.py --stage full --n 32 --sm 89 --spike-compat-nested-regext`

## 5) 下一步接手建议（按顺序）

1. 先做 develop：仅修 BF16 `C/D planner + D writeback` 与 TF32 split-n `C/D` 共享层，不扩散范围。
2. develop 完成后做只读 review，重点查 tuple planner 与 writeback 是否仍双轨或公式分叉。
3. review 收敛后再跑 test，要求失败面继续单调收缩，再决定是否可关任务 `26/29/32/33/35/36/38/40`。

## 6) 文档边界声明

- 本文档是 `active temporary handoff note`，用于接手执行，不直接改写 `current spec`。
- `current contract` 仍以 `openspec/specs/` 与 change artifact 已落地内容为准。
