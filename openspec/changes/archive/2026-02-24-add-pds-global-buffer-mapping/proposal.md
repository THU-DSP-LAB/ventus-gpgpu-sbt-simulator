## Why

当前 PTX 后端对 PDS（private data store / private memory）采用了“PTX local memory 临时模拟”路径：仅在遇到 `vlw.v/vsw.v` 时使用 `.local __sbt_pds[]` 作为 backing。该实现与 Ventus 软件栈的真实行为不一致：软件栈会在 global memory 分配一段足够大的 PDS buffer，并按 `block_id` 与 `warp_id` 为每个 warp 计算偏移形成 `CSR_PDS`。因此当前实现无法自然支持“普通访存地址落入 PDS buffer 即访问 PDS”的行为。

## What Changes

- **BREAKING**：扩展 SBT 生成的 PTX kernel ABI，使 runtime 传入 PDS 所需参数（`pdsBaseAddr` 与 `pdsSize`）。
- PTX 后端实现 `CSR_PDS` 的计算（按 Ventus 软件栈 / cyclesim 的公式）。
- 将 `vlw.v/vsw.v` 视为“普通向量访存 + PDS 偏移/布局规则”，统一落到数值地址空间映射（最终访问 global backing），移除 `.local __sbt_pds[]` 临时模拟路径。

## Capabilities

### New Capabilities
- `pds-addressing`: PDS 的 runtime 传参与 `CSR_PDS` 计算，以及 `vlw.v/vsw.v` 的全局 PDS buffer 访问语义

### Modified Capabilities
- (none)

## Impact

- Affected code:
  - PTX emitter：`sbt/ptx_emit.*`
  - （集成）Ventus driver PTX backend 参数传递：`ventus-env/driver/driver/ptx_device/ventus.cpp`
- Compatibility:
  - 需要同步更新 driver 端 `cuLaunchKernel` 的参数列表与顺序（PTX ABI 变更）。

