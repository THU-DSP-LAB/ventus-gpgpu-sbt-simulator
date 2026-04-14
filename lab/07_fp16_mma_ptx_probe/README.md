# lab/07_fp16_mma_ptx_probe

> 状态：`historical archived experiment`

本目录保留 `support-fp16-fp16-mma` change 落地前的前期实验记录。实验结论已同步进当前 `openspec/specs/inst-support/spec.md`、`README.md`、`tools/README.md` 与主线回归；本目录不再作为 current contract、current regression baseline 或 active 设计入口。

本目录只放“手写 PTX 对照 Spike”的隔离实验资产，不修改 `sbtsim` 主线 emitter / decode / tests。

## 目标

- 先验证 `m16n8k16 row.col fp16->fp16` 的手写 PTX 端到端路径。
- 输入保持随机，但限制在有限 `fp16` 值池，避免 NaN/Inf payload 干扰结论。
- 与现有 Spike 语义和 CPU reference 对齐，优先确认 PTX 侧的寄存器输入输出形态。
- 如果同一框架下自然可扩展到 `m16n16k16 = 2 x m16n8k16 split-n`，再顺手补上。

## 产物

- `probe_fp16_mma_ptx_ldmatrix.py`：主实验脚本，使用 CUDA Driver API 直接加载手写 PTX。
- `probe_fp16_mma_ptx.py`：早期探索脚本，保留作对照参考。
- `probe_fp16_mma_ventus_dump.py`：Ventus raw ABI 导出 + 无 `ldmatrix` PTX 对照脚本。
- `probe_fp16_mma_ptx_compat.py`：把 Ventus raw logical tile 兼容性变换成 PTX fragment 之后，再喂给 direct-lane `mma.sync` 的 probe。
- `probe_fp16_mma_ptx_direct_logical.py`：归档时已跑通的 no-`ldmatrix` direct-lane probe，输入为 logical `A/B/C` tile 内存。
- `probe_fp16_mma_ptx_direct_raw.py`：归档时已跑通的更直接 no-`ldmatrix` probe，输入保持 Ventus raw `A/B/C` window。
- `EXPERIMENT_REPORT.md`：实验完成后的结果摘要。

## 运行

```bash
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --help
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --seed 0x20260413 --keep-workdir
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ventus_dump.py --seed 0x20260413 --keep-workdir
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_compat.py --seed 0x20260413 --validate-fragments
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_logical.py --seed 0x20260413
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_raw.py --seed 0x20260413
```

## 口径

- Spike 对照沿用 `tools/fp16_mma_spike_cpu_ref.py` 的输入生成与 CPU reference 逻辑。
- 比较规则沿用同一脚本：
  - `NaN` 只看分类
  - 非 `NaN` 允许 `<= 1 fp16 ULP`
- PTX 侧通过 CUDA Driver API 直接加载手写 PTX，避免依赖 `sbt_ptx` 或主线 emitter。
- 归档时已验证的 PTX 物理布局要求：
  - A logical `16x16` tile 需要以转置后的 `16x16` 物理 shared 布局进入 `ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16`
  - B logical `8x16` tile 需要以转置后的 `16x8` 物理 shared 布局进入 `ldmatrix.sync.aligned.m8n8.x2.trans.shared.b16`
- 归档时已验证的 no-`ldmatrix` direct-lane 逻辑加载要求：
  - A logical `16x16` row-major：`base = group*32 + thread*4` bytes，寄存器偏移 `0 / 256 / 16 / 272`
  - B logical `8x16` row-major：`base = group*32 + thread*4` bytes，寄存器偏移 `0 / 16`
  - C/D logical `16x8` row-major：`base = group*16 + thread*4` bytes，寄存器偏移 `0 / 128`
- 归档时已验证的 no-`ldmatrix` direct-lane raw-window 直接映射要求：
  - A raw gather：`w = group*8 + thread + {0,64,4,68}`
  - B raw repack：`src_lane0 = thread*8 + (group>>1)`，`src_lane1 = src_lane0 + 4`，按 `group&1` 选 low/high half
  - C raw gather：当前 lane 的 `c.x/c.y`
  - D raw writeback：偶数 lane 取 `d0`，奇数 lane 取 `d1`
- Ventus raw ABI 也已通过 `probe_fp16_mma_ventus_dump.py` 实际导出并确认：
  - helper 生成的 `a/b/c/d` raw 窗口与 Ventus dump 一致
  - `probe_fp16_mma_ptx_compat.py` 已把 Ventus raw logical tile 精确重排成 PTX fragment ABI，并通过 fragment basis self-check
  - `probe_fp16_mma_ptx_direct_logical.py` 已进一步证明：不使用 `ldmatrix` 时，改为从 logical tile 内存按 direct-load offset 组装寄存器后，PTX/Spike/CPU ref 可以端到端一致
  - `probe_fp16_mma_ptx_direct_raw.py` 已进一步证明：可以不 materialize logical tile，直接从 Ventus raw `A/B/C` window 组装 PTX direct-lane 寄存器，并与 PTX/Spike/CPU ref 端到端一致
