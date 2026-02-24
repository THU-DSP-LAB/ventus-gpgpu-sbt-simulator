## Context

Ventus 软件栈会在 global memory 上分配 PDS buffer，并为每个 warp 计算 `CSR_PDS`（private memory base）。cyclesim 参考实现（用户提供）：

- warp 启动时设置 `CSR_PDS`（概念等价）：
  - `CSR_PDS = pdsBaseAddr + (blk_idx_in_kernel * num_warp_per_cta + warp_idx_in_blk) * num_thread_per_warp * pdsSize_per_thread`
- `vlw.v/vsw.v`（PDS 索引访存）根据 `req.pds_base` 与 lane 计算地址（概念等价）。

当前本项目 PTX 后端存在偏差：
- 使用 PTX `.local __sbt_pds[]` 模拟 `vlw.v/vsw.v` 的 backing（仅覆盖这两条指令）。
- 普通访存（标量/向量）走“shared/elf/heap”三段数值地址区间分流；若地址落在真实 PDS global buffer 中，理论上应该能访问到 PDS，但由于 `CSR_PDS` 与 `vlw.v/vsw.v` 语义未按软件栈实现，无法自然形成一致行为。

目标是将 PDS 作为“global backing 的一个区域”，使：
1) `CSR_PDS` 与软件栈一致；
2) `vlw.v/vsw.v` 通过数值地址映射访问 global PDS buffer（不再使用 PTX local 临时数组）；
3) 任何普通访存只要地址落在 PDS buffer 区间，就能访问到同一份数据。

## Goals / Non-Goals

**Goals:**
- 统一 PDS 的 backing：以 Ventus 软件栈分配的 global PDS buffer 为唯一来源。
- 明确并实现 `CSR_PDS` 的计算公式（按 block/warp 计算 warp base）。
- 将 `vlw.v/vsw.v` lowering 为“基于 `CSR_PDS` 的数值地址 + 现有地址映射”的 load/store。
- 维持现有 fail-fast 策略：遇到不支持的 PDS 相关指令/形态直接报错。

**Non-Goals:**
- 不新增诊断工具/子命令；不新增“未支持指令名列表输出”类报表。
- 不要求浮点 bit-level 一致性（本变更与浮点无关）。
- 不支持间接控制流（non-ret `jalr`）等既有结构化限制的放宽。

## Decisions

### Decision 1: 通过 kernel 参数传入 `pdsBaseAddr/pdsSize`（BREAKING ABI）
**Choice:** 扩展 PTX `.entry` 参数列表，runtime 直接传入：
- `pds_base_vaddr`：Ventus 数值地址（u32），指向 kernel 级 PDS buffer 的起始地址（global/heap 区间）
- `pds_size_per_thread`：每线程 private memory 字节数（u32）

**Rationale:**
- driver 侧 `vt_kernel_metadata_t` 已包含 `pdsBaseAddr/pdsSize`（`ventus-env/driver/include/ventus.h`），当前 `ptx_device` 仅未传入 PTX kernel。
- 与现有做法一致：`elf_base/heap_base/knl_vaddr` 已作为参数传入；PDS 同理由 runtime 提供。

**Alternative considered:** 将 `pdsBaseAddr/pdsSize` 写入 `CSR_KNL` 指向的 64B metadata buffer，再由 PTX 侧从 `knl_vaddr` 解引用读取。该方案避免 ABI 变更，但需要定义/对齐 metadata layout 且依赖额外访存；本变更优先选择更直接的参数传递。

### Decision 2: 将 PDS 置于 heap/global 数值地址区间，复用现有地址映射
**Choice:** 要求 `pds_base_vaddr >= heap_base_vaddr`（默认 `0x9000_0000`），从而落入现有 heap/global 分支；任何普通访存只要地址位于 PDS buffer 内，就可自然访问。

**Rationale:** 避免在通用访存路径中引入额外的 PDS 特判区间，保持地址空间模型简单。

### Decision 3: `vlw.v/vsw.v` 作为“普通向量访存 + PDS 布局规则”
**Choice:** `vlw.v/vsw.v` lowering 不再使用 PTX local backing；改为计算一个 Ventus 数值地址 `addr_vaddr_u32`，并调用现有的 `emit_addr_map_and_ld_u32/emit_addr_map_and_st_u32` 走 global backing。

**Addressing rule (e32):**
- 令 `NUMT = 32`（threads per warp）
- 令 `warp_base = CSR_PDS`（per-warp base，由 Decision 1 的参数 + block/warp 计算）
- 每 lane 的 `base_addr = vs1 + simm11`（byte offset）
- 访问地址：
  - `addr = warp_base + ((base_addr & ~3) * NUMT) + (laneid << 2)`
  - （注）该式与 cyclesim 中“按 lane 与 NUMT 做 interleave”的实现等价；也符合 `vlw.v/vsw.v` “比普通向量访存多一层 PDS 偏移/布局”的直观理解。

`CSR_PDS` 计算（per warp）：
- `blk_linear = ctaid.x + nctaid.x * (ctaid.y + nctaid.y * ctaid.z)`
- `warp_linear_in_kernel = blk_linear * warps_per_block + warp_id_in_block`
- `warp_base = pds_base_vaddr + warp_linear_in_kernel * (NUMT * pds_size_per_thread)`

## Risks / Trade-offs

- [Risk] PTX kernel ABI 变更导致 driver 未同步更新时 launch 失败 → Mitigation：在 change tasks 中明确 driver 更新步骤，并在回归中覆盖至少一个使用 `vlw.v/vsw.v` 的用例。
- [Risk] `pds_base_vaddr` 不在 heap/global 区间导致映射错误 → Mitigation：翻译侧做显式检查（不满足直接 fail-fast），并在文档中要求 runtime 保证地址区间。
- [Risk] `vsw.v` 的立即数编码宽度与解码不一致 → Mitigation：以 Spike/cyclesim 的 simm11 语义为准，修正 decode 的立即数提取规则（与 `vlw.v` 对齐）。

