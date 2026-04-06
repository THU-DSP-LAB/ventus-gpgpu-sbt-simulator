# MMA Lowering Architecture

> Status: `active`
>
> Role: active design contract for the repository-local Ventus MMA lowering path in `sbtsim`.
>
> This document is not a `current` implementation truth. It defines the intended lowering architecture, support partition, and mapping contract for Ventus MMA while `support-custom-mma` remains in progress. After the MMA path lands and the resulting behavior is synced into `openspec/specs/`, this document should either be downgraded to `historical` or rewritten as a short `current` pointer to the synced spec.

## Purpose

This document exists to freeze the architectural shape of Ventus MMA lowering before code implementation:

- how Ventus MMA enters the repository-local decode path,
- how MMA lowering remains isolated from already supported non-MMA Ventus instructions,
- how supported MMA shapes are partitioned into direct-native vs composite-lowered families,
- how Ventus VGPR windows are interpreted as logical MMA tiles,
- and how those logical tiles map to PTX native MMA operand fragments.

The main goal is to avoid a failure mode where PTX compile-first succeeds but the produced MMA operand mapping is semantically wrong.

## Status and Relationship to Other Docs

This document is an `active` design document.

It complements, but does not replace:

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/changes/support-custom-mma/*`

Responsibility split:

- `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` freezes cross-change prerequisites and shared baseline decisions.
- this document freezes the MMA-specific lowering architecture that is expected to remain useful even after the active change artifacts evolve.
- `openspec/changes/support-custom-mma/*` keeps the change-local proposal/design/tasks/spec delta.

## Design Goals

- Add Ventus MMA lowering without changing the behavior of already supported scalar, RVV, or custom non-MMA instructions.
- Keep the lowering architecture explicit enough that `Spike vs PTX` semantic validation can target the same logical contract.
- Support the first implementation batch needed by the current upstream toolchain and `ventus-pytorch` kernels.
- Make direct-native and composite MMA lowering share the same semantic middle layer instead of duplicating shape-specific ad hoc logic.

## Non-Interference Rule

Ventus MMA lowering must be introduced as an isolated family-specific path.

The implementation must preserve the behavior of all currently supported instructions, including:

- ordinary scalar and RVV lowering,
- current custom non-MMA families (`shuffle`, `vcvt`, packed arithmetic, approximate SFU),
- existing PTX module baseline assumptions (`.version 7.8`, `sm_89`),
- and existing non-MMA regression/oracle paths.

The required shape is:

```text
DecodedInst
  |- ordinary scalar / RVV / FP instructions
  |    -> existing lowering path
  |- current custom non-MMA families
  |    -> existing repository-local custom lowering path
  `- CustomFamily::Mma
       -> new MMA-specific lowering stack
```

This means:

- the generic `%vN`-based vector lowering model must not be redefined for non-MMA instructions,
- MMA-specific register-window / fragment concepts must stay inside the MMA lowering stack,
- unsupported MMA combinations must fail fast inside the MMA path instead of leaking partial behavior into generic lowering.

## Canonical Sources

The lowering architecture uses two different canonical sources for two different questions.

### 1. Ventus semantic source

Spike is the canonical source for Ventus MMA semantic interpretation:

- MMA field decode,
- per-shape register-window sizes,
- `A/B/C/D` logical tile load behavior,
- `n=16` block splitting behavior,
- and final `D` writeback behavior.

Relevant anchors include:

- `../spike/riscv/ventus_custom.cc`
- `../spike/riscv/encoding.h`

### 2. PTX legality source

PTX ISA plus `ptxas` compile-first behavior are the canonical sources for:

- whether a native PTX MMA form is legal on the shared `sm_89` baseline,
- which native shape/layout/type combinations are accepted directly,
- and what operand fragment tuple shape PTX requires for each native MMA form.

Spike is not the canonical source for PTX fragment legality, and PTX is not the canonical source for Ventus register-window semantics.

## First-Batch Scope

This document freezes the first implementation batch as the set of combinations that should be supported before broader MMA exploration.

### First-batch supported families

The first batch is partitioned by lowering family, not by a flat instruction list.

#### Family A: direct-native `m16n8k16 row.col`

- `m16n8k16 row.col f16 -> f16`
- `m16n8k16 row.col f16 -> f32`
- `m16n8k16 row.col bf16 -> f32`

Lowering class:

- one Ventus MMA instruction lowers to one PTX native `mma.sync`

#### Family B: direct-native `m16n8k8 row.col`

- `m16n8k8 row.col tf32 -> f32`

Lowering class:

- one Ventus MMA instruction lowers to one PTX native `mma.sync`

#### Family C: split-`n` composite `m16n16k16 row.col`

- `m16n16k16 row.col f16 -> f16`
- `m16n16k16 row.col f16 -> f32`
- `m16n16k16 row.col bf16 -> f32`

Lowering class:

- one Ventus MMA instruction lowers to two PTX native `m16n8k16 row.col` operations

#### Family D: split-`n` composite `m16n16k8 row.col`

- `m16n16k8 row.col tf32 -> f32`

Lowering class:

- one Ventus MMA instruction lowers to two PTX native `m16n8k8 row.col` operations

### Explicitly deferred in the first batch

The following families are deferred and must fail fast under `--require-known` until a later revision updates this document and the active change artifacts:

- all `m8*` shapes,
- all non-`row.col` layout combinations,
- all Spike-only combinations that are not currently exposed by the Ventus LLVM frontend surface,
- and any MMA path that would require `wmma` or another new native PTX class not already frozen by the first-batch matrix.

## Canonical Matrix

The canonical MMA support matrix is recorded here, next to the lowering-family
and mapping rules it depends on.

`openspec/changes/support-custom-mma/*` must summarize and depend on this
matrix. They must not carry a divergent second copy.

### Committed first-batch matrix

These are the only MMA combinations currently committed for implementation.

| Ventus shape | Layout | A/B -> C/D | Status | Lowering class | Native PTX building block | PTX baseline |
| --- | --- | --- | --- | --- | --- | --- |
| `m16n8k16` | `row.col` | `f16 -> f16` | `committed` | `direct-native` | `1 x mma.sync m16n8k16 row.col f16/f16/f16/f16` | `.version 7.8`, `sm_89` |
| `m16n8k16` | `row.col` | `f16 -> f32` | `committed` | `direct-native` | `1 x mma.sync m16n8k16 row.col f32/f16/f16/f32` | `.version 7.8`, `sm_89` |
| `m16n8k16` | `row.col` | `bf16 -> f32` | `committed` | `direct-native` | `1 x mma.sync m16n8k16 row.col f32/bf16/bf16/f32` | `.version 7.8`, `sm_89` |
| `m16n8k8` | `row.col` | `tf32 -> f32` | `committed` | `direct-native` | `1 x mma.sync m16n8k8 row.col f32/tf32/tf32/f32` | `.version 7.8`, `sm_89` |
| `m16n16k16` | `row.col` | `f16 -> f16` | `committed` | `split-n composite` | `2 x mma.sync m16n8k16 row.col f16/f16/f16/f16` | `.version 7.8`, `sm_89` |
| `m16n16k16` | `row.col` | `f16 -> f32` | `committed` | `split-n composite` | `2 x mma.sync m16n8k16 row.col f32/f16/f16/f32` | `.version 7.8`, `sm_89` |
| `m16n16k16` | `row.col` | `bf16 -> f32` | `committed` | `split-n composite` | `2 x mma.sync m16n8k16 row.col f32/bf16/bf16/f32` | `.version 7.8`, `sm_89` |
| `m16n16k8` | `row.col` | `tf32 -> f32` | `committed` | `split-n composite` | `2 x mma.sync m16n8k8 row.col f32/tf32/tf32/f32` | `.version 7.8`, `sm_89` |

### Frontend-exposed but uncommitted inventory

The current Ventus LLVM frontend exposes a larger surface than the committed
matrix above. Those combinations are intentionally kept as inventory only: they
must fail fast today, and this document does not pretend that their eventual
lowering strategy is already known-correct.

| Frontend-exposed family group | Currently exposed combinations | Status | Current planning state | Notes |
| --- | --- | --- | --- | --- |
| `m16n8k16` with non-`row.col` layouts | `row.row`, `col.row`, `col.col` for `f16 -> f16`, `f16 -> f32`, `bf16 -> f32` | `deferred` | `TODO` | requires non-`row.col` native/composite mapping study |
| `m16n8k8` with non-`row.col` layouts | `row.row`, `col.row`, `col.col` for `tf32 -> f32` | `deferred` | `TODO` | outside the committed `row.col` acceptance surface |
| `m16n16k16` with non-`row.col` layouts | `row.row`, `col.row`, `col.col` for `f16 -> f16`, `f16 -> f32`, `bf16 -> f32` | `deferred` | `TODO` | would need a non-`row.col` composite contract |
| `m16n16k8` with non-`row.col` layouts | `row.row`, `col.row`, `col.col` for `tf32 -> f32` | `deferred` | `TODO` | would need a non-`row.col` composite contract |
| `m8n8k16` and `m8n16k16` | all frontend-exposed layouts for `f16 -> f16`, `f16 -> f32`, `bf16 -> f32` | `research` | `TODO` | likely needs `wmma` or shape-lift composite work |
| `m8n8k8` and `m8n16k8` | all frontend-exposed layouts for `tf32 -> f32` | `research` | `TODO` | likely needs `wmma` or shape-lift composite work |

## Layered Lowering Model

The first-batch architecture is frozen as a four-layer model.

### Layer 1: `MmaInstInfo`

`MmaInstInfo` is the structured decode product for Ventus MMA.

It is frozen as a dedicated metadata object hanging off `DecodedInst`, not as a
shape/layout overloading of `CustomInstInfo` and not as an emitter-only raw-bit
blob.

The generic ownership marker remains:

- `DecodedInst.custom.valid = true`
- `DecodedInst.custom.family = CustomFamily::Mma`

But MMA-specific consumers must read `DecodedInst.mma`, not reinterpret
`custom.subop` / `custom.dtype` as shape/layout/type carriers.

For the first batch, `MmaInstInfo` should be explicit enough that later layers
do not need to re-parse raw instruction bits. At minimum it should include:

- `bool valid`
- `MmaShape shape`
- `MmaLayout a_layout`
- `MmaLayout b_layout`
- `MmaAbType ab_type`
- `MmaCdType cd_type`
- `bool spike_a_column_layout`
- `bool spike_b_row_layout`
- `int rd_base`
- `int rs1_base`
- `int rs2_base`
- `uint8_t a_regs_per_thread`
- `uint8_t b_regs_per_thread`
- `uint8_t c_regs_per_thread`
- `bool wide_ab`
- `FirstBatchMmaClass support_class`

This layer answers only:

- what Ventus MMA instruction was encoded,
- and what per-thread register windows it owns.

It must not decide PTX lowering strategy by itself.

### Layer 2: `MmaLoweringPlan`

`MmaLoweringPlan` decides how one decoded Ventus MMA instruction is lowered.

For the first batch, the supported plan kinds are:

- `DirectNativeM16N8`
- `SplitNInto2xM16N8`

The plan must define:

- whether the instruction lowers directly or compositionally,
- the `PtxMmaAbiKey` used by each emitted native sub-operation,
- the PTX native shape/type/layout used by each emitted sub-operation,
- and which logical `n`-subtile each sub-operation is responsible for.

This layer must not directly emit PTX text.

### Layer 3: `VGPR window -> logical tile`

This is the semantic core of the architecture.

The lowering path must explicitly interpret the Ventus register windows as logical matrix tiles before any PTX tuple emission happens.

Conceptually:

```text
Ventus MMA instruction
  -> rd / rs1 / rs2 base VGPR windows
  -> per-lane register payloads
  -> logical A / B / C / D tile view
```

This layer must define, for each supported family:

- which lane-local register slots belong to logical `A`,
- which belong to logical `B`,
- which belong to logical `C` / accumulator input,
- how `row.col` layout affects logical `(row, col)` interpretation,
- and how logical `D` maps back to the `rd` window.

This layer must be semantically equivalent to Spike's `load_matrix_a`, `load_matrix_b_block`, `load_matrix_c_block`, and `store_matrix_d` behavior.

### Layer 4: `logical tile -> PTX fragment tuple`

Only after a logical tile is defined may the lowering path construct PTX operand tuples.

This layer is PTX-specific and must define:

- the native PTX operand ABI descriptor for each first-batch native form,
- the operand fragment register order for each native PTX MMA form,
- the fragment slice consumed by each composite sub-operation,
- and the result tuple writeback order for each native or composite sub-operation.

This layer must not reinterpret Ventus semantics. It only translates an already-defined logical tile into a PTX-native form.

For the first batch, the implementation contract is:

- every emitted native `mma.sync` is keyed by one explicit `PtxMmaAbiKey`,
- each `PtxMmaAbiKey` has one explicit `PtxMmaAbiDesc`,
- operand tuples are materialized from the logical tile through dedicated helper logic,
- and neither direct-native nor split-`n` lowering may treat raw VGPR window order as an implicit PTX tuple ABI.

## First-Batch Lowering Invariants

The following invariants are frozen for the first batch.

### Direct-native families

For Family A and Family B:

- one source Ventus MMA instruction must produce exactly one native PTX MMA operation,
- no synthetic rows, columns, or accumulator fragments may be invented,
- no `wmma` path may be selected,
- and no fallback to non-MMA arithmetic decomposition is allowed.

### Split-`n` composite families

For Family C and Family D:

- one source Ventus MMA instruction must produce exactly two native PTX MMA operations,
- both sub-operations must share the same logical `A` tile,
- the only legal first-batch composite split is along logical `n`,
- sub-operation 0 and sub-operation 1 must cover disjoint `n` ranges,
- and the combined writeback must reconstruct the original logical `16 x 16` output tile without overlap or silent truncation.

### Unsupported families

For explicitly deferred families:

- the repository-local decode path may recognize them as MMA,
- but the lowering path must fail fast with an explicit unsupported classification,
- and it must not silently reinterpret them as a supported first-batch family.

## Why the Logical Tile Layer Is Mandatory

The project must not jump directly from `VGPR window` to `PTX fragment tuple`.

Without a logical-tile middle layer, the implementation would collapse into shape-specific register-splicing code. That would make it hard to:

- review semantic correctness against Spike,
- validate composite `n=16 -> 2 * n=8` lowering,
- prove that PTX operand tuples match the intended Ventus tile,
- and catch compile-supported but semantically wrong fragment orderings.

`ptxas` can validate PTX tuple legality. It cannot validate whether the tuple contains the correct Ventus matrix elements.

## Formal Mapping Rules (First Batch)

This section freezes the first-batch mapping rules in a form that is directly
derivable from Spike's current MMA execution semantics in
`../spike/riscv/ventus_custom.cc`.

It defines the three required layers:

- VGPR window summary
- VGPR window / lane-slot -> logical tile
- logical tile -> PTX fragment tuple

The rules are written so implementers can build:

- `MmaInstInfo` (structured metadata),
- `MmaLoweringPlan` (direct vs split-`n`),
- and emitter helpers (operand tuple construction and writeback).

### Shared Definitions

#### Notation

- `lane`: the lane id in `[0, 31]`
- `reg`: the per-thread register-slot index inside a VGPR window
- `base`: the base VGPR number of a contiguous window
- `u32(base + reg, lane)`: read the 32-bit value stored in VGPR `(base + reg)` at `lane`

#### Shape Parameters

Spike defines the per-shape window sizes as:

```text
shape m16n8k16:  aRegsPerThread=4  bRegsPerThread=2  cRegsPerThread=4
shape m16n8k8:   aRegsPerThread=4  bRegsPerThread=2  cRegsPerThread=4
shape m16n16k16: aRegsPerThread=4  bRegsPerThread=4  cRegsPerThread=8
shape m16n16k8:  aRegsPerThread=4  bRegsPerThread=4  cRegsPerThread=8
```

These are the canonical per-thread VGPR window sizes for the first batch.

#### Element Kind (`wide_elem`)

Spike distinguishes two A/B input element modes:

- `wide_elem = true`: TF32 inputs (one element per `u32`)
- `wide_elem = false`: FP16/BF16 inputs (two 16-bit elements packed into one `u32`)

Packed input convention:

- `u32 = lo16 | (hi16 << 16)`
- `lo16` and `hi16` are two consecutive logical elements in the linear index

#### Layout Bits (Spike Interpretation)

Spike interprets the instruction bits as:

- `alayout_bit` is passed as `column_layout` for A
- `blayout_bit` is passed as `row_layout` for B

For the first batch, only `row.col` variants are committed. Under Spike's
interpretation, `row.col` means:

- A is row-major: `column_layout = false`
- B is col-major (expressed as B^T row-major in the `n x k` view): `row_layout = true`

If later evidence shows the frontend encoding uses a different bit polarity,
the decoder MUST expose booleans that match Spike's interpretation above (do not
silently flip semantics inside the emitter).

### Layer 1: VGPR Window Summary

For any first-batch MMA instruction:

- A window: `rs1_base + [0 .. aRegsPerThread-1]`
- B window: `rs2_base + [0 .. bRegsPerThread-1]`
- C input window: `rd_base + [0 .. cRegsPerThread-1]`
- D output window: `rd_base + [0 .. cRegsPerThread-1]` (overwrites C)

This matches Spike's use of `rs1_base`, `rs2_base`, and `rd_base` for
load/accumulate/store in `ventus_exec_mma`.

### Layer 2: VGPR Window / Lane-Slot -> Logical Tile (Spike-Equivalent)

This layer is frozen exactly as Spike's current mapping.

#### A tile mapping (all first-batch shapes)

Let `shape.m`, `shape.k` be the MMA shape parameters.

For each `reg in [0 .. aRegsPerThread-1]` and `lane in [0 .. 31]`:

- Read `value = u32(rs1_base + reg, lane)`
- If `wide_elem`:
  - Define `idx = reg * 32 + lane`
  - Store one logical A element at linear index `idx` with payload `value`
- Else:
  - Define `idx0 = reg * 64 + lane * 2`
  - Store two logical A elements:
    - element at linear index `idx0` with payload `value & 0xffff`
    - element at linear index `idx0 + 1` with payload `value >> 16`

Then interpret linear index `idx` as `(m, k)`:

- If `column_layout`:
  - `k = idx / shape.m`
  - `m = idx % shape.m`
- Else (row-major):
  - `m = idx / shape.k`
  - `k = idx % shape.k`

Only indices with `m < shape.m` and `k < shape.k` are in-range.

#### B tile mapping (all first-batch shapes)

Let `shape.n`, `shape.k` be the MMA shape parameters.

For each `reg in [0 .. bRegsPerThread-1]` and `lane in [0 .. 31]`:

- Read `value = u32(rs2_base + reg, lane)`
- If `wide_elem`:
  - Define `idx = reg * 32 + lane`
  - Store one logical B element at linear index `idx` with payload `value`
- Else:
  - Define `idx0 = reg * 64 + lane * 2`
  - Store two logical B elements:
    - element at linear index `idx0` with payload `value & 0xffff`
    - element at linear index `idx0 + 1` with payload `value >> 16`

Then interpret linear index `idx` as `(n, k)` in the `n x k` view:

- If `row_layout`:
  - `n = idx / shape.k`
  - `k = idx % shape.k`
- Else:
  - `k = idx / shape.n`
  - `n = idx % shape.n`

For split-`n` composite lowering, a sub-operation with `col_offset` uses only
entries with:

- `col_offset <= n < col_offset + 8`
- `k < shape.k`

and stores them into a local `n' = n - col_offset` range `[0,7]`.

#### C/D accumulator tile mapping (all first-batch shapes)

For each `reg in [0 .. cRegsPerThread-1]` and `lane in [0 .. 31]`:

- Read `value = u32(rd_base + reg, lane)`
- Define `idx = reg * 32 + lane`
- Interpret `idx` as `(m, n)`:
  - `m = idx / shape.n`
  - `n = idx % shape.n`

For split-`n` composite lowering, a sub-operation with `col_offset` uses only
entries with:

- `col_offset <= n < col_offset + 8`

and stores them into a local `n' = n - col_offset` range `[0,7]`.

Writeback of `D` uses the same `(reg, lane) -> (m, n)` mapping and overwrites
the `rd_base` window slots.

### Layer 3: Logical Tile -> PTX Fragment Tuple

This layer freezes the *register-tuple construction rule* for the PTX emitter.
It does not invent a second semantic mapping: it only states how the VGPR
window is passed as PTX operand tuples.

#### Native PTX ABI descriptors

The first batch freezes four native ABI keys. Each emitted native `mma.sync`
must name one of them explicitly.

| `PtxMmaAbiKey` | Native PTX form | A tuple regs/lane | B tuple regs/lane | C tuple regs/lane | D tuple regs/lane |
| --- | --- | --- | --- | --- | --- |
| `M16N8K16_F16_F16` | `mma.sync m16n8k16 row.col f16/f16/f16/f16` | `4` | `2` | `2` | `2` |
| `M16N8K16_F16_F32` | `mma.sync m16n8k16 row.col f32/f16/f16/f32` | `4` | `2` | `4` | `4` |
| `M16N8K16_BF16_F32` | `mma.sync m16n8k16 row.col f32/bf16/bf16/f32` | `4` | `2` | `4` | `4` |
| `M16N8K8_TF32_F32` | `mma.sync m16n8k8 row.col f32/tf32/tf32/f32` | `4` | `2` | `4` | `4` |

The tuple counts above are frozen by the native PTX operand contract and match
the currently exposed Ventus LLVM tuple-window ABI for the corresponding
families. They are part of the implementation contract for first-batch
lowering.

Each `PtxMmaAbiDesc` must define at least:

- the native PTX form string,
- A/B/C/D tuple arity per lane,
- the element packing mode (`packed16x2` or `wide32`),
- and the helper entry points that materialize operand tuples from a logical tile
  and merge native results back into the logical `D` tile.

#### Operand materialization policy

The first batch intentionally uses one explicit operand-materialization policy
for both direct-native and split-`n` paths:

- `materialize_a_tuple(logical_a, abi_key, n_slice)`
- `materialize_b_tuple(logical_b, abi_key, n_slice)`
- `materialize_c_tuple(logical_c, abi_key, n_slice)`
- `merge_d_tuple(logical_d, native_d, abi_key, n_slice)`

For direct-native families, `n_slice` is the full native tile.

For split-`n` families, `n_slice` is either `[0, 7]` or `[8, 15]` in logical
Ventus coordinates.

This means the implementation is not allowed to have a "direct-native shortcut"
that bypasses tuple materialization just because the VGPR window sizes happen to
match the PTX tuple arity. If an optimized identity path is later proven
correct, it may be introduced as a derived implementation optimization, but it
must preserve the same explicit ABI descriptor contract.

#### Direct-native families (A/B)

For Family A (`m16n8k16 row.col`) and Family B (`m16n8k8 row.col`):

- The PTX emitter MUST lower to one native `mma.sync` with the corresponding
  `m16n8k16` or `m16n8k8` shape and `row.col` layout.
- For FP16/BF16 families, each `u32` register carries two packed 16-bit payloads.
  The element ordering within the packed `u32` is the Spike-equivalent ordering
  defined in Layer 2.

Direct-native implementation contract:

- The tuple ABI is frozen by the corresponding `PtxMmaAbiDesc`, not by raw
  VGPR window order.
- `A/B/C` tuples must be materialized from the logical tiles using the explicit
  helper layer above, even if the resulting implementation is a pure slot copy.
- Any later shortcut that reuses VGPR window order directly must be justified as
  an optimization of the same ABI contract and remain covered by the
  Spike-backed oracle.

#### Split-`n` composite families (C/D)

For Family C (`m16n16k16 row.col`) and Family D (`m16n16k8 row.col`):

- The Ventus instruction defines one logical `m16 x n16` output tile.
- Lowering emits exactly two PTX native `m16 x n8` `mma.sync` operations:
  - sub-op 0: `col_offset = 0`
  - sub-op 1: `col_offset = 8`
- Both sub-ops share the same logical A tile (Layer 2 A mapping).
- Each sub-op consumes only the `n in [col_offset, col_offset+7]` slice of B and
  C and produces the matching slice of D (Layer 2 block rules).

Formal operand construction constraints:

- Both sub-ops share the same logical A tile and therefore consume the same
  logical A slice.
- B/C/D tuples for each sub-op MUST be constructed so that the PTX operand ABI
  represents exactly the logical `n`-slice described above.

Known fully-frozen pieces from Spike:

- The `n`-slice itself (which logical elements belong to which sub-op) is frozen
  by the Layer 2 block rules.
- For the B window under the first-batch `row.col` layout, the block split is
  also representable as a contiguous register split:
  - `m16n16*` uses `bRegsPerThread = 4`
  - each `m16n8*` sub-op uses `bRegsPerThread = 2`
  - therefore:
    - sub-op 0 uses `B_tuple = %v(rs2_base + {0,1})`
    - sub-op 1 uses `B_tuple = %v(rs2_base + {2,3})`

Composite tuple instantiation rules:

- For split-`n`, the implementation must reuse the same native `PtxMmaAbiDesc`
  helpers as the direct-native families, one invocation per `n_slice`.
- `B_tuple` may be selected by contiguous register-half for the committed
  first-batch `row.col` forms as described above.
- `C_tuple` materialization and `D_tuple` merge are explicit repack operations
  over the logical `n_slice`, not implicit aliasing of the larger `m16n16*`
  accumulator window.
- Any required lane-wise moves/shuffles remain part of the explicit tuple
  materialization helpers. They are not fallback behavior and must stay visible
  in code and validation.

## Expected Integration Points

The first-batch architecture should integrate into the codebase through MMA-specific touch points rather than broad emitter rewrites.

### Decode layer

Expected touch points:

- `sbt/riscv_decode.hpp`
- `sbt/riscv_decode.cpp`

Expected direction:

- extend `DecodedInst` with structured `MmaInstInfo` metadata,
- keep `CustomFamily::Mma` as the family discriminator,
- keep current non-MMA `CustomInstInfo` consumers unchanged,
- and avoid overloading `CustomSubOp` / `CustomDataType` with MMA-only fields.

### PTX lowering layer

Expected touch points:

- `sbt/ptx_emit.cpp`
- optionally a future MMA-specific helper file if the PTX emitter becomes too large

Expected direction:

- add a dedicated MMA lowering entry path,
- keep MMA-specific tuple construction out of the generic non-MMA vector lowering path,
- and encode first-batch support partitioning in explicit plan kinds rather than scattered instruction-name string checks.

### Validation layer

Expected touch points:

- `tools/custom_decode_test.cpp`
- a new MMA PTX compile-first test
- a new MMA Spike-vs-PTX oracle tool or test entry
- `tools/regress.sh`

Expected direction:

- retain current non-MMA test behavior unchanged,
- keep `tools/custom_decode_test.cpp` as the non-MMA gate until MMA decode is
  actually implemented there,
- add a dedicated MMA decode/compile/oracle test path rather than mutating the
  non-MMA gate into a mixed gate,
- and make first-batch MMA support verifiable independently of deferred MMA families.

### Documentation layer

Expected touch points:

- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/changes/support-custom-mma/*`
- eventually `openspec/specs/inst-support/spec.md`

Expected direction:

- this document stays `active` while implementation is in progress,
- the code map must describe the as-built MMA lowering stack once it exists,
- and the synced spec must later carry the `current` support contract after implementation lands.

## Deferred Mapping Classes

The following mapping classes are intentionally out of scope for the first batch because their semantic and lowering risks are materially higher.

### 1. `m8*` shape lifting

`m8*` shapes are deferred because they would require:

- lifting an `m=8` logical tile into a larger native `m=16` PTX shape,
- synthesizing zero-valued rows for the upper half,
- and writing back only the logically valid result rows.

That is not just block-splitting. It is shape promotion plus partial writeback.

### 2. Non-`row.col` layout remapping

Non-`row.col` layouts are deferred because they would require at least one of:

- a different native PTX acceptance surface,
- a layout-remapping fragment transformation,
- or another composite lowering path with materially different mapping rules.

The first batch deliberately avoids mixing those problems into the initial architecture.

## Validation Contract

The first-batch validation contract is layered to match the lowering architecture.

### Compile-first validation

Compile-first must prove:

- the PTX native MMA forms are accepted on `sm_89`,
- the emitted PTX syntax and tuple shape are valid,
- and direct-native and composite forms both assemble cleanly.

Compile-first does not prove semantic correctness of matrix-element placement.

### Semantic validation

Semantic validation must compare Spike-backed execution results against the PTX path for supported first-batch families.

The validation path must be able to catch at least:

- `A/B/C/D` window slicing mistakes,
- swapped or reversed `n` subtile composition,
- incorrect tuple ordering inside one native PTX MMA invocation,
- and incorrect `D` writeback back into the Ventus destination window.

### Bring-up cross-check

During bring-up, a repository-local helper model may still be used as an additional cross-check, but it must not replace Spike-backed comparison as the canonical semantic oracle.

## Expected Implementation Shape

The architecture described here implies the following code-structure direction:

- repository-local MMA decode extends `DecodedInst` with structured MMA metadata,
- the PTX emitter adds a dedicated MMA lowering entry point,
- support partitioning is encoded as explicit plan kinds rather than implicit name-based branching,
- and any future `wmma` or `m8*` exploration should plug into the same `MmaLoweringPlan -> logical tile -> PTX fragment` stack instead of bypassing it.

This direction is intentionally narrower than a general-purpose PTX fragment framework. The first batch should add only the abstractions needed to make the supported MMA families explicit and testable.

## Change Control

The following changes require updating this document before implementation proceeds:

- expanding the first-batch support surface beyond the families listed above,
- adding `wmma` as a supported lowering class,
- adding any `m8*` or non-`row.col` family,
- redefining the `VGPR window -> logical tile` model,
- or changing the canonical Spike-vs-PTX semantic validation rule for MMA.
