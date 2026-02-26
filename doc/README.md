# doc/ 索引（实现文档）

本目录未来主要维护三份长期文档（尽量不过期）：
- `doc/README.md`：本索引。
- `doc/IMPLEMENTATION_CODEMAP.md`：目录/模块职责、关键数据结构与调用链（按源码入口落盘）。
- `doc/IMPROVEMENT_PROPOSALS.md`：当前实现的主要问题清单与“向通用化演进”的改进方案（不改代码，仅建议）。

历史阶段交接/实测快照已归档在 `doc/archive/`（用于回溯当时的“as-built”结论与日志路径）。

## 1. ISA/ABI 语义备忘

- `doc/ventus-isa/csr.md`：原型期涉及的 CSR 列表与备注（以仓库语义约定为准）。
- `doc/ventus-isa/vbranch_simtstack.md`：`setrpc/vbranch/join` 的 SIMT stack 语义。
- `doc/ventus-isa/others.md`：regext、mask、barrier、数值地址空间等杂项约定。

## 2. 归档（阶段交接 / 带日期快照）

- `doc/archive/STATUS_SBT_PIPELINE_2026-02-22.md`：近期变更摘要与回归结果（2026-02-22）。
- `doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`：阶段 4（PoCL/driver 集成、ptx_device + SBT PTX JIT）交接说明。
- `doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md`：SBT（Ventus ELF → decode → CFG verify → PTX）流水线梳理与实测结果（2026-02-19）。

## 3. 相关但不在 doc/ 的材料

- `README.md`：项目目标与命令用法（用户入口）。
- `lab/`：历史归档实验目录（不再作为当前实现入口）。
- `openspec/`：OpenSpec 变更提案与阶段 gate（已沉淀部分能力到 `openspec/specs/`；历史变更归档在 `openspec/changes/archive/`）。
