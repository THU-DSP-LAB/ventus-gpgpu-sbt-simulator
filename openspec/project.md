# Project Context

## Purpose
本项目面向乘影（Ventus）开源 GPGPU（RISC-V + V 扩展语义重解释）的原型探索：尝试将 Ventus ISA 做静态二进制翻译（SBT）为 NVIDIA PTX，从而在 NVIDIA GPU 上进行更快速的功能级仿真与验证。

阶段性目标（原型期）：
- 梳理/固化指令语义来源（以 `VentusInst_basic.xlsx` + 补充文档为准）
- 打通最小端到端链路：Ventus 指令流 → 解码/翻译 → PTX → `ptxas`/运行验证
- 建立“可重复验证”的对齐方法：小样例/微基准覆盖关键指令与控制流语义

## Tech Stack
- 输入：Ventus ISA 指令表与二进制/指令流（当前主要参考 `VentusInst_basic.xlsx`）
- 输出：NVIDIA PTX（必要时进一步产物为 cubin + SASS 用于检查）
- CUDA 工具链（必需）：`nvcc`、`ptxas`、`cuobjdump`、`nvdisasm`
- 实现语言（待定，建议）：
  - 原型/研究阶段：Python（便于快速迭代与解析 Excel/生成文本 PTX）
  - 稳定/性能阶段：C++ 或 Rust（便于构建高性能解码/IR/生成器）
- 运行环境（待定）：NVIDIA GPU + 对应 CUDA Toolkit；本仓库里已有 `sm_89`（RTX 4090）相关实验记录可参考 `useless/uniform_sass_lab`

## Project Conventions

### Code Style
- 文档：Markdown；中文描述为主，PTX/SASS/SIMT/RVV 等术语保留英文缩写
- Shell 脚本：`set -euo pipefail`；输出写入明确的 `build/` 或 `out/` 目录；可重复运行
- 若使用 Python（建议约定）：
  - 格式化/静态检查：`black` + `ruff`
  - 命名：`snake_case`；模块/文件名同样使用 `snake_case`
- 若使用 C++（建议约定）：
  - 格式化：`clang-format`
  - 明确禁止 UB；优先使用可验证的位操作/解码方式
- 变更与规格：涉及“新增能力/语义改变/架构变化”的工作，优先走 OpenSpec（见 `openspec/AGENTS.md`）

### Architecture Patterns
建议按“解码 → 中间表示（IR）→ 降低（lowering）→ PTX 生成”的流水线组织：
- Decoder：将 Ventus 指令编码解析为结构化指令对象（字段、寄存器、立即数、类别）
- Semantics/IR：把标量/向量/自定义指令统一表达（便于做正确性验证与多后端）
- PTX Lowering：将 IR 映射到 PTX（必要时引入约束以满足 `ptxas`）
- 待定，是否真的需要IR？
关键语义划分（来自项目背景，详见 `README.md`）：
- 标量路径：类似 warp-uniform（可参考 NVIDIA uniform datapath 的行为特征）
- 向量路径：per-thread 操作（对应 SIMT 下的普通指令）
- 分支/收敛：按 Ventus 的 SIMT stack 语义实现（如 `vbranch`/`setrpc`/`join` 等；具体方案 TBD）

### Testing Strategy
原型期建议以“可对齐、可复现”为核心：
- 解码测试：字段提取、非法编码、以及必要时的 encode/decode round-trip
- 指令语义测试：为关键指令准备 micro-kernel/小样例；运行 PTX 结果与参考模型对比（参考模型 TBD：可为简单解释器/已有仿真器）
- 回归产物：保留生成的 PTX 文本、运行输出、以及必要的 `cuobjdump --dump-sass` 片段做对照（避免工具链升级导致悄然变化）

### Git Workflow
- `main` 保持“随时可运行/可复现”
- 分支命名：`feat/...`、`fix/...`、`docs/...`
- 提交建议：Conventional Commits（`feat:`/`fix:`/`docs:`/`refactor:` 等），小步提交便于回滚与审阅
- 规格驱动：需要“新增能力/重要语义调整/架构调整”时先写 OpenSpec change，再实现代码

## Domain Context
- Ventus ISA：标量部分接近 `RV32IMA_zicsr_zfinx`；向量部分基于 RVV 但语义有重解释（例如不使用 RVV mask、`v0` 为普通向量寄存器；分支/收敛使用自定义指令）
- 目标翻译后端：NVIDIA PTX（并关注 PTX → SASS 的落地行为差异）
- 关注点：SIMT 控制流语义、warp-uniform vs per-thread 的区分、以及如何用 PTX 表达 Ventus 的自定义指令语义

## Important Constraints
- 原型阶段优先功能正确与可验证性，不追求性能最优
- 指令表（`VentusInst_basic.xlsx`）含合并单元格：若做自动解析需要特别处理
- PTX 必须满足 `ptxas` 约束（控制流、寄存器、地址空间等），并在目标 `sm_*` 上可编译运行（目标架构 TBD）
- 部分指令可阶段性不支持，但需明确“支持范围/行为（trap、stub、未实现）”的约定（TBD）

## External Dependencies
- CUDA Toolkit：`nvcc`、`ptxas`、`cuobjdump`、`nvdisasm`
- （可选，若用 Python 解析 Excel）：`openpyxl`/`pandas`（TBD）
- （可选，参考行为来源）：Ventus 仿真器/RTL/其他可执行参考（TBD）
