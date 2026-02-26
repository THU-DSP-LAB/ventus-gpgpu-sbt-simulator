# Change: Add SBT bring-up plan for Rodinia Ventus ELF (ELF → decode/CFG → PTX)

## Why
当前仓库已完成了 SBT 可行性分析与若干实验（`lab/03_*`、`lab/04_*`），但还缺少一个“可审核、可执行、可验收”的落地计划，把这些分析收敛为分阶段交付物与明确的支持/不支持边界。

下一阶段目标是：让系统能够**逐步**正确处理 `ventus-env/rodinia/opencl/*/*.riscv` 这批 Ventus RISC-V ELF 输入，并最终具备端到端执行验证能力（通过 PoCL/driver 或独立 runner）。

## What Changes
- 固化原型阶段的输入约束、失败策略与非目标（fail-fast，而不是隐式降级）。
- 将 SBT 管线拆为可独立验收的阶段：
  - ELF/`.text` 提取 + 指令解码（含 `regext` 前缀合并）
  - CFG 构建 + `setrpc/vbranch/join` 结构化验证
  - PTX 生成（含 warp-uniform 标量语义与地址空间路由）+ `ptxas` 可编译
  - 可选：与 `ventus-env` driver 的 `ptx_device` 集成，跑通 Rodinia kernel
- 为每个阶段定义最小产物（CLI/JSON/日志）与验收用例（基于仓库现成的 Rodinia ELF + `.dump`）。

## Impact
- Affected inputs:
  - `ventus-env/rodinia/opencl/*/*.riscv`
  - `ventus-env/rodinia/opencl/*/*.dump`（用于对照/验证）
- Affected areas:
  - OpenSpec change 文档（本变更）
  - 本仓库内的 SBT 管线实现与工具（现状已包含 decode/CFG verify/PTX emit，以及可选的 driver 集成与回归脚本；本变更用于将其目标/边界/验收口径文档化）。
- Non-goals (prototype stage):
  - 全指令集覆盖、完整 ELF 重定位、不可结构化 CFG 的兜底（software SIMT stack/PC dispatch）、原子/一致性/缓存语义、浮点/除法等 corner case 的位级一致性。

## Status note (based on current repo state)
本 change 最初表述为“计划/规格收敛”，但当前仓库代码与文档已经具备对应实现与交付物（包括 `sbt_decode`/`sbt_ptx`、CFG 结构化校验 JSON、以及 PoCL/driver 的 `ptx_device` 运行路径说明）。

因此本 change 的“完成”以“仓库已具备上述阶段产物与可复现流程”为准；端到端执行与数值正确性仍需在具备 CUDA/GPU 的本机环境中验证（受运行环境影响，可能无法在受限沙箱内复现）。
