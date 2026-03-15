# lab/（归档）

本目录已整体归档，仅保留历史探索记录与复盘材料，不再作为当前实现、测试或回归入口。

当前有效入口请使用仓库根目录 `README.md` 与 `doc/README.md`。

历史子目录说明：
- `00_prototype`：手工 Ventus ISA -> PTX 最小验证
- `01_host_device_abi`：手工验证 Ventus kernel ABI 与 PTX 兼容性
- `02_host_device_abi_integrate`：早期与 `ventus-env` driver 集成实验
- `03_sbt_feasibility`：SBT 可行性分析与草稿
- `04_instruction_decode`：早期解码设计记录
- `05_ptx_direct_call_param_abi`：围绕 direct call 的 `vctx` vs `.param value ABI` 微基准实验
- `06_ptx_call_boundary_dead_state`：围绕 call-boundary dead state 是否会被 `ptxas` 消去的实验背景与目标草案
- `others`：外围调研记录
