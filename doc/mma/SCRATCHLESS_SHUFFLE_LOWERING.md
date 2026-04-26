# MMA Scratchless Shuffle Lowering

> Status: `current`
>
> Role: 当前 committed MMA lowering 的 scratchless shuffle materialization/writeback 实现说明。
>
> Current implementation truth: `sbt/ptx_emit_internal.hpp` 中的 MMA path 已不再使用 `.shared` scratch
> staging；A/B/C tuple materialization 与 D writeback 通过 `shfl.sync.idx.b32`、固定候选寄存器选择和
> packed-half merge 完成。更宽 MMA matrix 的长期架构入口仍是 `doc/mma/LOWERING_ARCHITECTURE.md`。

## 背景

当前 sbtsim 已支持 8 条 first-batch MMA family：

| Ventus family | 当前 lowering |
| --- | --- |
| `m16n8k16 row.col f16 -> f16` | 1 条 native PTX `mma.sync.m16n8k16` |
| `m16n8k16 row.col f16 -> f32` | 1 条 native PTX `mma.sync.m16n8k16` |
| `m16n8k16 row.col bf16 -> f32` | 1 条 native PTX `mma.sync.m16n8k16` |
| `m16n8k8 row.col tf32 -> f32` | 1 条 native PTX `mma.sync.m16n8k8` |
| `m16n16k16 row.col f16 -> f16` | 2 条 native PTX `mma.sync.m16n8k16`，按 `n` 切成 `[0,7]` / `[8,15]` |
| `m16n16k16 row.col f16 -> f32` | 2 条 native PTX `mma.sync.m16n8k16`，按 `n` 切成 `[0,7]` / `[8,15]` |
| `m16n16k16 row.col bf16 -> f32` | 2 条 native PTX `mma.sync.m16n8k16`，按 `n` 切成 `[0,7]` / `[8,15]` |
| `m16n16k8 row.col tf32 -> f32` | 2 条 native PTX `mma.sync.m16n8k8`，按 `n` 切成 `[0,7]` / `[8,15]` |

这些 family 的共同点是：

- Ventus 输入/输出以 per-lane VGPR window 表示。
- PTX `mma.sync` 要求每个 lane 持有固定 ABI 的 fragment tuple。
- 当前实现不能直接把 raw VGPR window 当作 PTX tuple ABI，必须经过 `VGPR window -> logical tile -> PTX fragment tuple`。

## 需求

目标是记录当前不依赖 `.shared` MMA scratch 的数据重排实现，使 committed MMA family 的 lowering 仍满足现有语义合同：

- 不改变支持范围：只覆盖当前 committed `row.col m16*` family。
- 不引入 mock、fallback 或静默降级；unsupported family 仍 fail-fast。
- 保持显式 tuple ABI descriptor，不回退到 raw VGPR window order。
- 消除对 `KNL_LDS_STACK_SIZE_PER_WF` 起始区域的隐式占用。
- 仍能覆盖 direct-native 和 split-`n` composite 两类路径。

## 旧实现背景

当前实现已经不再使用下面的 staging 流程；这里保留旧流程用于解释 scratchless lowering 替换了什么。

旧 `emit_native_mma_sync()` 的 staging 流程是：

1. 计算 per-warp MMA scratch base：

   ```text
   scratch_ptr = __sbt_shmem + warp_id_in_block * ldsStackSizePerWf
   ```

2. 把 A window、B window、C/D carrier window 写入 `.shared` scratch：

   ```text
   scratch[reg][lane] = %v(base + reg)
   ```

3. 对每个 PTX tuple slot，根据 `ScalarTupleValue` 先算 logical coordinate，再转回 source `(reg, lane, half)`，从 scratch 中 `ld.shared.u32`。
4. 执行 native PTX `mma.sync`。
5. 把 native D tuple 写回 scratch 的 D window。
6. 从 scratch reload 回 `%v(rd_base + reg)`。

这个旧实现简单、通用，但有两个结构性问题：

- 它使用 Ventus LDS stack 区作为翻译器私有 staging，边界不够干净。
- 每次 MMA 都引入大量 shared store/load 和 warp sync，性能和资源占用偏重。

## 核心观察

当前 committed MMA 的 tuple mapping 都是 lane-local 可计算的固定置换/重排。

对每个 PTX lane `L` 和 tuple slot，`ScalarTupleValue` 给出 logical coordinate：

```text
effective_lane = L xor lane_xor_mask
logical_row    = (effective_lane >> lane_row_shift) + tile_row_base
logical_col    = ((effective_lane & lane_col_mask) << lane_col_shift)
                 + tile_col_base + lane_col_bias
```

再根据 role 转换为 Ventus source window 的 linear index。

A/C/D 的通用转换：

```text
A row-major:
  idx = logical_row * shape_k + logical_col

C/D:
  idx = logical_row * shape_n + (logical_col + n_slice_offset)
```

B 的转换需要使用 B 的 `(n,k)` view：

```text
B row_layout:
  idx = (logical_n + n_slice_offset) * source_window_k + logical_k

B col_layout:
  idx = logical_k * source_window_n + (logical_n + n_slice_offset)
```

当前 committed `row.col` 解码下，B 使用 col-layout source rule（即 `MmaLayout::Col` / `spike_b_row_layout=false`）；
split-`n` 通过 `n_slice_offset = 0` 或 `8` 选择 native sub-op 对应的 `n` 区间。

linear index 到 Ventus source carrier 的转换：

```text
wide32 / f32 / tf32:
  source_reg  = idx >> 5
  source_lane = idx & 31
  source_half = none

packed fp16/bf16:
  source_reg  = idx >> 6
  source_lane = (idx >> 1) & 31
  source_half = idx & 1
```

这些 `source_lane` 都在同一个 warp 内。因此 `.shared` scratch 的跨 lane gather 可以替换为 `shfl.sync.idx.b32`。

## 方案概览

当前 scratchless MMA materializer 保留现有 plan/ABI 层：

```text
DecodedInst.mma
  -> AbiDesc / ScalarTupleValue plans
  -> shuffle-based A/B/C tuple materialization
  -> mma.sync
  -> shuffle-based D tuple merge/writeback
```

不改变：

- `MmaInstInfo`
- `AbiDesc`
- `ScalarTupleValue`
- support-class / lowering-class 边界
- direct-native 与 split-`n` 的 native `mma.sync` 数量

已替换掉的旧 helper：

- `emit_spill_v_window_to_mma_scratch`
- `emit_load_*_from_spilled_window`
- `emit_store_d_tuple_to_spilled_cd_window`
- `emit_reload_v_window_from_mma_scratch`

当前 helper 形态：

- `emit_materialize_tuple_regs_by_shuffle`
- `emit_materialize_b_tuple_regs_by_shuffle`
- `emit_merge_d_tuple_to_v_window_by_shuffle`

## A/B/C Tuple Materialization

### 1. 计算 tuple slot 的 source descriptor

对每个 tuple slot 或 packed tuple element：

```text
logical_coord = tuple_plan(role, tuple_reg, tuple_elem, current_lane)
idx           = logical_coord_to_source_index(role, logical_coord, n_slice_offset)
source_reg    = idx >> reg_shift
source_lane   = lane extraction from idx
source_half   = optional packed half selector
```

`reg_shift` 是：

- packed fp16/bf16：`6`
- wide32/f32/tf32：`5`

### 2. 用 `shfl.sync.idx.b32` gather source lane

PTX `shfl.sync.idx.b32` 的 source operand 必须是一个明确寄存器，不能用 runtime `source_reg` 间接索引 `%v(base + source_reg)`。因此 materializer 应按候选 source reg 生成固定 shuffle，再选择需要的值。

伪代码：

```text
candidate0 = shfl.idx(%v(base + 0), source_lane)
candidate1 = shfl.idx(%v(base + 1), source_lane)
candidate2 = shfl.idx(%v(base + 2), source_lane)
candidate3 = shfl.idx(%v(base + 3), source_lane)

word = select(source_reg, candidate0, candidate1, candidate2, candidate3)
```

候选数量由 role/window 决定：

| Role | native `m16n8*` | split `m16n16*` source |
| --- | --- | --- |
| A | 4 regs | 4 regs |
| B | 2 regs | 4 regs，因为 second `n` slice 仍可能从完整 B source window 取数 |
| C f16 carrier | 2 regs | 4 regs |
| C f32 carrier | 4 regs | 8 regs |

选择可以用 predicated `mov` 或 small switch-style conditional moves 表达。候选数量很小，避免了 shared memory。

### 3. packed half extract / repack

packed fp16/bf16 tuple 需要从 source word 取 half：

```text
lo = word & 0xffff
hi = word >> 16
elem = source_half ? hi : lo
```

每个 PTX tuple register 若为 packed16x2，则分别 materialize lo/hi element 后重新打包：

```text
tuple_reg = lo_elem | (hi_elem << 16)
```

wide32/f32/tf32 tuple 直接使用 gathered word；f32 accumulator tuple 用 `.f32` temp 时仍通过 `mov.b32` 保持 raw bits。

## D Tuple Writeback

D writeback 是旧 scratch 方案最依赖 shared scatter 的部分。scratchless 实现反过来做 destination-side gather：

1. 对每个 destination carrier `%v(rd_base + reg)` 和当前 lane `L`，计算该 carrier 当前 lane 对应的 logical D coordinate。
2. 判断该 coordinate 是否属于当前 native sub-op 的 `n_slice`。
3. 计算 producer lane 和 D tuple slot。
4. 当前 lane 用 `shfl.sync.idx.b32` 从 producer lane 的 D tuple register 拉取结果；如果 D tuple slot 不是编译期常量，
   必须像 A/B/C source-reg selection 一样先对每个候选 D tuple register 生成固定 shuffle，再用 predicate 选择。
5. 写回当前 lane 的 destination `%v`；packed fp16 需要按 half 合并。

### f32/bf16/tf32 D writeback

对于 f32 accumulator family，每个 destination word 对应一个 logical D element：

```text
idx = dest_reg * 32 + current_lane
m   = idx / shape_n
n   = idx % shape_n
```

如果 `n` 不属于当前 sub-op 的 `[n_slice_offset, n_slice_offset + 7]`，这个 sub-op 不更新该 destination word。

否则：

```text
local_n = n - n_slice_offset
```

当前 ABI 的 f32 D plans 是：

| D tuple reg | row block | col parity |
| --- | --- | --- |
| 0 | rows 0..7 | even local col |
| 1 | rows 0..7 | odd local col |
| 2 | rows 8..15 | even local col |
| 3 | rows 8..15 | odd local col |

逆映射：

```text
row_in_block = m & 7
col_pair     = local_n >> 1
producer_lane = (row_in_block << 2) | col_pair
tuple_reg =
  (m >= 8 ? 2 : 0) + (local_n & 1)
```

然后对所有候选 D tuple register 生成固定 shuffle，再按 `tuple_reg` 选择。native D tuple 是 `.f32` temp 时，需要先
`mov.b32` 到 `.b32` temp 后再执行 `shfl.sync.idx.b32`，写回 `%v` 时继续保持 raw bits。

```text
candidate0 = shfl.idx(bitcast_b32(d_tuple[0]), producer_lane)
candidate1 = shfl.idx(bitcast_b32(d_tuple[1]), producer_lane)
candidate2 = shfl.idx(bitcast_b32(d_tuple[2]), producer_lane)
candidate3 = shfl.idx(bitcast_b32(d_tuple[3]), producer_lane)
result_word = select(tuple_reg, candidate0, candidate1, candidate2, candidate3)
%v(rd_base + dest_reg) = result_word
```

### f16 D writeback

fp16 D carrier 每个 word 有两个 half：

```text
idx0 = dest_reg * 64 + current_lane * 2
idx1 = idx0 + 1
```

分别对 `idx0/idx1` 计算：

```text
m = idx / shape_n
n = idx % shape_n
```

若 `n` 属于当前 sub-op slice，则从 D tuple 拉取该 half：

```text
local_n       = n - n_slice_offset
row_in_block  = m & 7
col_pair      = local_n >> 1
producer_lane = (row_in_block << 2) | col_pair
tuple_reg     = (m >= 8 ? 1 : 0)
tuple_elem    = local_n & 1
```

拉取时也不能动态索引 D tuple operand；需要先对候选 packed D tuple register 生成固定 shuffle，再选择 half：

```text
candidate0    = shfl.idx(d_tuple[0], producer_lane)
candidate1    = shfl.idx(d_tuple[1], producer_lane)
packed_result = select(tuple_reg, candidate0, candidate1)
half_result   = tuple_elem ? (packed_result >> 16) : (packed_result & 0xffff)
```

写回时必须保留不属于当前 sub-op 的 half：

```text
old_word = %v(rd_base + dest_reg)
new_lo   = slice_updates_lo ? lo_result : old_word.lo
new_hi   = slice_updates_hi ? hi_result : old_word.hi
%v(rd_base + dest_reg) = new_lo | (new_hi << 16)
```

当前 `m16n16k16 f16 -> f16` 的 packed carrier 以偶/奇 `n` 成对，slice 边界在 `7/8`，因此同一个
32-bit word 的两个 half 不会跨越两个 split-`n` sub-op。这里仍建议逐 half 合并，因为它直接表达 slice
ownership，避免未来调整 carrier layout 或扩展 family 时把“不属于本 sub-op 的 half”误覆盖。

## Direct-Native Family 规则

direct-native family 的 `n_slice_offset = 0`，每条 Ventus MMA 只执行一次 native PTX `mma.sync`。

### `m16n8k16 row.col f16 -> f16`

- A：4 个 packed16x2 tuple regs，从 `rs1 + [0..3]` gather。
- B：2 个 packed16x2 tuple regs，从 `rs2 + [0..1]` gather。
- C：2 个 packed16x2 tuple regs，从 `rd + [0..1]` carrier gather。
- D：2 个 packed16x2 tuple regs，按 fp16 D writeback 写回 `rd + [0..1]` carrier。

### `m16n8k16 row.col f16 -> f32`

- A：4 个 packed16x2 tuple regs，从 `rs1 + [0..3]` gather。
- B：2 个 packed16x2 tuple regs，从 `rs2 + [0..1]` gather。
- C：4 个 f32 tuple regs，从 `rd + [0..3]` gather。
- D：4 个 f32 tuple regs，按 f32 D writeback 写回 `rd + [0..3]`。

### `m16n8k16 row.col bf16 -> f32`

与 `f16 -> f32` 相同，区别是 A/B packed payload 被 PTX `mma.sync ... bf16.bf16.f32` 解释为 BF16。数据搬运仍是 raw packed16x2。

### `m16n8k8 row.col tf32 -> f32`

- A：4 个 wide32 tuple regs，从 `rs1 + [0..3]` gather。
- B：2 个 wide32 tuple regs，从 `rs2 + [0..1]` gather。
- C：4 个 f32 tuple regs，从 `rd + [0..3]` gather。
- D：4 个 f32 tuple regs，按 f32 D writeback 写回 `rd + [0..3]`。

## Split-`n` Family 规则

split-`n` family 每条 Ventus MMA 执行两次 native PTX MMA：

```text
sub-op 0: n_slice_offset = 0
sub-op 1: n_slice_offset = 8
```

A tuple 两个 sub-op 完全相同。B/C/D tuple 根据 `n_slice_offset` 选择对应 logical `n` slice。

### `m16n16k16 row.col f16 -> f16`

- A：每个 sub-op 从 `rs1 + [0..3]` gather。
- B：每个 sub-op 从完整 `rs2 + [0..3]` source window gather，`n_slice_offset` 决定取 `[0,7]` 或 `[8,15]`。
- C：每个 sub-op 从 `rd + [0..3]` carrier gather。
- D：每个 sub-op 只更新本 slice 对应的 half，最终合并到 `rd + [0..3]`。

### `m16n16k16 row.col f16 -> f32`

- A：每个 sub-op 从 `rs1 + [0..3]` gather。
- B：每个 sub-op 从完整 `rs2 + [0..3]` gather。
- C：每个 sub-op 从 `rd + [0..7]` gather，仅使用本 slice。
- D：每个 sub-op 只更新本 slice 对应的 `%v(rd + reg)` word。

### `m16n16k16 row.col bf16 -> f32`

与 `f16 -> f32` 相同，A/B raw payload 为 packed BF16。

### `m16n16k8 row.col tf32 -> f32`

- A：每个 sub-op 从 `rs1 + [0..3]` gather。
- B：每个 sub-op 从完整 `rs2 + [0..3]` gather，source pack 为 wide32。
- C：每个 sub-op 从 `rd + [0..7]` gather，仅使用本 slice。
- D：每个 sub-op 只更新本 slice 对应的 `%v(rd + reg)` word。

## 预期收益

### 1. 去掉对 Ventus LDS stack 的隐式占用

旧 scratch 放在：

```text
__sbt_shmem + warp_id * ldsStackSizePerWf
```

这等价于借用了 Ventus `_start` ABI 的 per-warp LDS stack 区。shuffle-based 实现不再写这块内存，避免与 xgpr spill stack、未来 ABI 调整或用户可见 shared 地址空间边界产生隐式耦合。

### 2. 减少 shared memory traffic 和同步

旧 staging 每次 native MMA 都需要：

- spill A/B/C window 到 shared
- materialize tuple 时大量 `ld.shared`
- D tuple 写回 shared
- reload D window
- 多次 warp sync

shuffle-based 实现把这些替换为 register shuffle、select、bit extract/pack。它仍会增加寄存器压力和指令数，但不会占用 shared bandwidth，也不需要为 staging 做 shared memory fence/sync。

### 3. 让翻译器私有中间产物真正留在 PTX register domain

MMA tuple 是 lowering 中间产物，不是 Ventus architectural memory state。scratchless 实现更符合这个边界：中间值只存在于 PTX virtual temps 和 native MMA tuple regs 中。

### 4. 保留现有语义分层

当前实现继续消费 `AbiDesc` / `ScalarTupleValue`，不把 raw VGPR window order 当 ABI shortcut。这样可以继续复用现有 mapper/oracle，并让未来 deferred family 的扩展仍按显式 ABI descriptor 接入。

## 风险与注意事项

- 寄存器压力会上升：每个 tuple slot 可能需要为多个 candidate source reg 生成 shuffle 结果，再做 select。
- 代码生成复杂度会上升：尤其是 split-`n` 的 D merge 和 fp16 packed half 保留逻辑。
- 需要精确处理 active mask：`shfl.sync.idx.b32` 的 producer lane 必须在 member mask 内。当前实现已在 MMA native
  sub-op 入口读取 `activemask` 并对非 `0xffffffff` 的 full-active-warp 情况显式 `trap`；只把当前 active mask 传给
  `shfl.sync` 不能自动保证正确性。
- 需要避免引入“看似优化、实则跳过 logical tile”的 direct-copy shortcut。
- 对 fp16 split-`n`，推荐逐 half 判断 slice ownership；当前 committed layout 中 half pair 不跨 slice，但实现不应依赖
  这个事实去引入难以审查的粗粒度 shortcut。

## 当前验证

当前至少保留以下验证：

- `build/mma_ptx_emit_test`：检查 emitted PTX 不再包含 MMA 专用 `st.shared.u32` / `ld.shared.u32` staging。
- `tools/custom_mma_oracle.py --stage full --sm 89`：继续覆盖 8 条 current supported family 的 Spike / sbtsim PTX / CPU reference 三方比较。
- 针对 split-`n f16 -> f16` 增加专门 case，覆盖 destination-side gather、逐 half 合并以及未归属当前 slice 的 carrier 保留。
- 增加 compile/static 检查，确认 D writeback 没有动态 tuple operand shortcut，而是使用固定候选 shuffle + predicate/select。
- full-warp 前提验证：`mma_ptx_emit_test` 检查 emitted PTX 包含 active-mask 检查与 trap；三方 oracle 以完整
  32-lane warp 执行 current MMA kernels。
- ptxas compile-first gate：确认 shuffle/select/temp 声明在 `.version 7.8` / `sm_89` 下合法。

## 结论

当前 committed MMA lowering 已不再使用 `.shared` scratch。所有已支持 family 的 A/B/C tuple materialization 和 D writeback 都使用 warp-level `shfl.sync.idx.b32`、小规模 source-reg select、half extract/pack 以及 destination-side gather 表达。

该方案的主要价值不是扩大功能，而是把 lowering 私有中间产物从 Ventus 可见 LDS stack 区移回 PTX register/shuffle domain，降低 ABI 耦合和 shared memory 成本。代价是实现复杂度与寄存器压力上升，需要用现有 MMA oracle 保持逐 family 语义闭环。
