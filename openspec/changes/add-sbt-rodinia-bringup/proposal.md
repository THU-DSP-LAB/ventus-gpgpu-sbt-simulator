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
  - 后续实现预计会新增 SBT 前端/后端代码与测试（**本变更不实现代码**，仅提出计划与规格）。
- Non-goals (prototype stage):
  - 全指令集覆盖、完整 ELF 重定位、不可结构化 CFG 的兜底（software SIMT stack/PC dispatch）、原子/一致性/缓存语义、浮点/除法等 corner case 的位级一致性。

