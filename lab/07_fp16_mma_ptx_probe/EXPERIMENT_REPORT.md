# fp16 MMA PTX probe report

> 状态：`historical archived experiment report`
>
> 本报告记录 `support-fp16-fp16-mma` change 落地前的 probe 结果。其结论已被吸收进 current spec、current 文档与主线回归；本文件仅保留 historical 证据价值。

## 结论

归档时，`m16n8k16 row.col f16->f16` 已有三条端到端跑通的手写 PTX 路径，且都与 Spike / CPU ref 完全一致：

- `ldmatrix` 路径：
  - `A` 先转成物理转置布局，再进入 `ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16`
  - `B` 先转成物理转置布局，再进入 `ldmatrix.sync.aligned.m8n8.x2.trans.shared.b16`
  - `C/D` 按 logical row-major 口径处理
- no-`ldmatrix` direct-lane 路径：
  - 不再从 fragment-packed window 直接 load
  - 改为从 logical `A/B/C` tile 内存按 direct-load offset 组装寄存器
  - 随后执行 `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16`
- raw-window direct-lane 路径：
  - 输入保持 Ventus raw `A/B/C` window
  - `A/C` 直接 gather
  - `B` 在 PTX kernel 中从 raw vertical-pair word 重打包成 PTX horizontal-pair `b0/b1`
  - 随后执行同一条 `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16`

关键收敛点有两个：

- 早期 direct-lane probe 的 `D` store 寻址是错的，会发生跨 lane 覆盖，因此才会出现“只剩 33 个 word 非零”的假象
- 即使修好 `D` store，`A/B` 仍不适合直接从 fragment-packed window 连续 load；归档时稳定可复现的 no-`ldmatrix` 方案已经覆盖 raw-window 直接映射和 logical-tile 兼容映射两条路径

## 已验证命令

```bash
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --seed 0x20260413 --keep-workdir
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_ldmatrix.py --seed 0x20260413 --load-mode cubin
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_logical.py --seed 0x20260413
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_logical.py --seed 0x20260413 --load-mode cubin
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_raw.py --seed 0x20260413
python3 lab/07_fp16_mma_ptx_probe/probe_fp16_mma_ptx_direct_raw.py --seed 0x20260413 --load-mode cubin
```

## 关键结果

### 随机输入

Seed `0x20260413` 下，`ldmatrix`、direct-logical 和 direct-raw 三条路径都满足：

- `finite_pairs=64`
- `finite_pass=64`
- `max_abs_err=0.0`
- `max_ulp_err=0`

### `ldmatrix` 物理布局探针

在 basis probe 中确认了以下事实：

- 仅把 logical tile 原样拷进 shared 会失败
- `A` 的 physical transpose 是必要条件
- `B` 的 physical transpose 也是必要条件

### Ventus raw ABI dump

`probe_fp16_mma_ventus_dump.py` 进一步确认：

- Ventus dump 的 `a/b/c` raw 窗口和 helper 生成的窗口一致
- Ventus dump 的 `d` raw 窗口和 CPU ref 一致
- 无 `ldmatrix` 的“fragment-packed 直连变体”全部失败，最接近的候选也只有 `1/64` finite pass

### fragment 兼容性 transform

`probe_fp16_mma_ptx_compat.py` 进一步确认：

- host 侧把 Ventus raw logical tile 重排成 PTX fragment ABI 的规则已经钉住
- basis 输入下，A/B/C 的 PTX fragment packing 与 `ldmatrix` 路径逐字节一致
- 这条 probe 的价值是把“fragment ABI 本身”钉死；但它也说明，仅有 fragment-packed window 还不足以得到正确的 no-`ldmatrix` direct-lane 结果

### no-`ldmatrix` direct logical-load

`probe_fp16_mma_ptx_direct_logical.py` 最终给出了端到端正确结果：

- 输入不是 fragment-packed window，而是 logical tile 内存
- `A` row-major `16x16`：
  - `base = group*32 + thread*4` bytes
  - `a0/a1/a2/a3` 偏移 `0 / 256 / 16 / 272`
- `B` row-major `8x16`：
  - `base = group*32 + thread*4` bytes
  - `b0/b1` 偏移 `0 / 16`
- `C/D` row-major `16x8`：
  - `base = group*16 + thread*4` bytes
  - `c0/d0, c1/d1` 偏移 `0 / 128`
- 在 `--seed 0x20260413` 下，`ptx` / `cubin` 两种加载模式都通过：
  - logical `D` tile 与 CPU ref 逐 half 完全一致
  - pack 回 Ventus raw `D` window 后，与 Spike / CPU ref 也完全一致

### no-`ldmatrix` direct raw-window

`probe_fp16_mma_ptx_direct_raw.py` 进一步把兼容变换收缩到了 raw-window 级别：

- 输入保持 Ventus raw `A/B/C` window
- `A` raw gather：
  - 令 `w = group*8 + thread`
  - `a0/a1/a2/a3` 分别取 raw word index `w + {0,64,4,68}`
- `B` raw repack：
  - `src_lane0 = thread*8 + (group>>1)`
  - `src_lane1 = src_lane0 + 4`
  - `group&1 == 0` 时取 low half，`group&1 == 1` 时取 high half
  - 用这两个 half 分别组装 `b0` 和 `b1`
- `C` raw gather：
  - 当前 lane 的 `c.x/c.y`
- `D` raw writeback：
  - 偶数 lane 写 `d0`
  - 奇数 lane 写 `d1`
- 在 `--seed 0x20260413`、`0x1`、`0x12345678` 下，`ptx` 模式全部通过
- 在 `--seed 0x20260413` 下，`cubin` 模式也通过

## 还未验证

- `m16n16k16 f16->f16` 的 `split-n` 扩展还没有在同一 probe 里端到端确认
- 本 report 只覆盖 `m16n8k16 row.col f16->f16`
- “直接以 Ventus raw window 为 kernel 输入”的正确 no-`ldmatrix` 方案已经得到
- 仍未得到“直接以 fragment-packed raw window 为 kernel 输入”的正确 no-`ldmatrix` 方案

## 备注

该 probe 只在隔离目录内工作，不修改 sbtsim 主线 emitter / decode / tests。
