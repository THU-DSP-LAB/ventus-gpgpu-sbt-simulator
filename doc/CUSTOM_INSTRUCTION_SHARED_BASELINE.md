# Custom Instruction Shared Baseline

> Status: `active`
>
> Role: shared active design baseline for the `support-custom-instructions` and `support-custom-mma` changes.
>
> This document is not a `current` contract. It freezes the cross-change prerequisite decisions that both active changes must implement against. After both changes land and sync into `openspec/specs/`, this document should be archived or downgraded to `historical`.

## Purpose

Freeze the shared baseline for:

- project-wide PTX target selection,
- semantic oracle strategy,
- non-MMA custom canonical semantics,
- MMA initial support subset and lowering boundary,
- and the ownership rules between the two active custom-instruction changes.

Without this shared baseline, the two active changes would otherwise be free to make conflicting decisions about `sm_XX`, oracle path, packed semantics, and MMA scope during implementation.

## Inputs and Scope

This document consolidates decisions based on:

- `doc/CUSTOM_INSTRUCTION_INPUT.md` as input material, not as canonical current contract,
- current project constraints in `README.md`, `doc/IMPLEMENTATION_CODEMAP.md`, `openspec/project.md`, and `openspec/specs/inst-support/spec.md`,
- local `ptxas 13.1` compile-first probes performed on 2026-03-29 and 2026-03-31,
- and NVIDIA PTX ISA documentation for `mma.sync`, `wmma`, `bf16x2`, and related target requirements.

This document covers shared prerequisites only. It does not replace:

- `support-custom-instructions` ownership of shared custom decode framework and non-MMA lowering,
- or `support-custom-mma` ownership of MMA-specific decode semantics, support matrix details, and validation closure.

## Frozen Decisions

### 1. Project-wide PTX baseline

The shared project-wide PTX baseline for the planned custom-instruction support is frozen as:

- `.version 7.8`
- `.target sm_89`

Rationale:

- The planned non-MMA scope explicitly includes packed `bf16x2` arithmetic and packed `bf16x2` SFU, and that scope is still retained.
- Local `ptxas 13.1` probes on 2026-03-29 and 2026-03-31 confirmed that:
  - `add.bf16x2` and `mul.bf16x2` require `sm_90+`
  - `ex2.approx.ftz.bf16x2` requires `sm_90+`
  - `fma.rn.bf16x2` is accepted on `sm_89`
  - `cvt.rn.f32.bf16`, `cvt.rn.bf16.f32`, and `cvt.rn.bf16x2.f32` are accepted on `sm_89`
  - the first committed `mma.sync` subset for `m16n8k16` / `m16n8k8` `row.col` is accepted on `sm_89`
- Therefore the project can keep the planned `bf16x2` and MMA scope on an `sm_89` baseline only if packed `bf16x2` support is specified as a mixed native/composite lowering strategy instead of assuming `sm_90`-only native arithmetic and SFU opcodes.
- This keeps the shared baseline aligned with currently available validation hardware instead of freezing the whole custom-instruction plan behind unavailable `sm_90` runtime hardware.

Implications:

- `sm_75` compatibility is no longer a goal for the active custom-instruction changes.
- Current docs, command examples, regression defaults, and runtime assumptions that still use `sm_75` remain `current` only until the active changes land; they must be updated together during implementation rather than via custom-only exceptions.
- The integrated runtime path must migrate together with this repo. In particular, the default SM selection in `../driver/driver/ptx_device/ventus.cpp` must stop clamping to `75`, or integrated PoCL/driver execution will diverge from the frozen `sm_89` project baseline even if compile-first passes in this repo.
- The first implementation phase must treat explicit composite `bf16x2` lowering as part of the baseline contract, not as an optional fallback that can be skipped when native `sm_90` opcodes are unavailable.

### 2. Canonical semantic oracle path

The canonical semantic oracle policy for the active custom instructions is frozen as an explicit family-scoped rule:

- for custom instruction families that are already supported by the current Spike/Ventus OpenCL software stack, the canonical semantic oracle is Spike-backed OpenCL buffer comparison;
- for custom instruction families that are outside the current Spike-backed support surface, the canonical oracle remains a repository-managed reference model.

This avoids two failure modes:

- silently treating compile-first success as semantic validation,
- and forcing already Spike-supported families to maintain a second independent oracle path without a concrete benefit.

Constraints:

- Any repository-managed reference model must remain implementation-independent from PTX lowering.
- It must not reuse the core semantic helpers in a way that would make the oracle and lowering fail in the same way for the same bug.
- Any Spike-backed oracle path must still make outputs observable through OpenCL buffers and compare them explicitly, rather than relying on implicit device success.

External anchors:

- Native PTX behavior, local compile-first probes, and vendor documentation are strongly recommended as spot-check anchors.
- They are not an implementation gate for every instruction family.

### 3. Non-MMA canonical semantics

The following non-MMA decisions are frozen before implementation.

#### 3.1 Packed `f16x2` / `bf16x2`

- Packed custom instructions use a 32-bit container interpretation.
- One active vector element corresponds to one 32-bit packed container.
- `vl` counts packed containers, not individual 16-bit halves.
- These instructions are not required to preserve the mental model of ordinary `vsew=16` element-wise vector execution.
- On the shared `sm_89` baseline, packed `bf16x2` support uses a mixed lowering strategy:
  - `vfma.bf16x2` may map to native `fma.rn.bf16x2`
  - `vadd.bf16x2` / `vmul.bf16x2` may lower through `bf16 -> f32 -> fp32 op -> bf16`
  - packed `bf16x2` SFU may lower through `bf16 -> f32 -> fp32 SFU/composed sequence -> bf16`

#### 3.2 Packed FMA aliasing rule

For `vfma.f16x2` and `vfma.bf16x2`:

- canonical semantics are `vd = vs1 * vs2 + old_vd`
- the instruction reads the old accumulator value before writing the new result
- overlapping register usage is legal and must observe read-old-then-write-new semantics

#### 3.3 Shuffle

- `shuffle.idx/up/down/bfly` semantics align with the corresponding PTX `shfl.sync.*` behavior
- the project does not introduce a second, more abstract custom shuffle semantic distinct from the PTX warp-shuffle model

#### 3.4 Approximate SFU

For `fp32`, `f16x2`, and `bf16x2` custom SFU instructions:

- the semantic contract is mathematical-function intent plus approximation tolerance
- the project does not require bit-exact agreement with one specific implementation
- lowering should map to the closest native PTX instruction where available
- where native PTX is missing, explicit composed lowering is allowed
- on the shared `sm_89` baseline, packed `bf16x2` SFU support is expected to use explicit convert/compute/repack lowering rather than `sm_90`-only native PTX SFU opcodes

#### 3.5 Vector convert

For custom `vcvt` instructions:

- semantics are element-wise convert
- rounding mode is fixed to `rn`
- this shared baseline does not freeze additional corner-case detail beyond consistency with the chosen reference model

### 4. MMA lowering boundary and initial supported subset

The project recognizes three MMA lowering classes:

1. `native-mma-sync`
2. `native-wmma`
3. `composite-lowering`

The initial implementation commitment is frozen as:

- first supported subset: `native-mma-sync` only
- `native-wmma`: active research target, not part of the first committed subset
- `composite-lowering`: allowed as a later direction, not part of the first committed subset

#### 4.1 Native PTX candidate space

The project must not restrict native MMA exploration to `mma.sync` alone.

Both of the following are recognized as legitimate native PTX candidate paths:

- `mma.sync`
- `wmma`

However, being a native PTX candidate does not by itself make a Ventus MMA combination supported. Each supported combination still requires:

- an accepted PTX form,
- a stable Ventus register-window to PTX fragment mapping,
- and semantic validation against the chosen oracle path.

#### 4.2 First committed MMA subset

The initial MMA subset committed by the shared baseline is:

- `m16n8k16 row.col f16 -> f16`
- `m16n8k16 row.col f16 -> f32`
- `m16n8k16 row.col bf16 -> f32`
- `m16n8k8 row.col tf32 -> f32`

These four entries are chosen because they cover the input-material dtype families while staying inside the most plausible `native-mma-sync` support surface.

For the first committed `native-mma-sync` subset:

- PTX-side native layout is frozen as `row.col`
- therefore the first committed Ventus layout subset is also frozen to combinations that can map to PTX `row.col`
- other layout pairs are not part of the first implementation commitment and must fail fast unless and until this shared baseline is revised

#### 4.3 Explicitly non-committed shapes

The following input-material shapes are not part of the first committed subset:

- `m8n8k16`
- `m8n16k16`
- `m16n16k16`
- `m8n8k8`
- `m8n16k8`
- `m16n16k8`

They are not declared impossible forever, but they are not part of the first implementation commitment in this baseline. In particular:

- some may require `wmma` rather than `mma.sync`
- some may require composite tiling, padding, or decomposition
- none may be silently treated as if they were already covered by the first committed subset

## Local Probe Notes

The following local compile-first facts were confirmed with `ptxas 13.1` on 2026-03-29 and 2026-03-31:

- `.version 7.8` is the first probed PTX ISA version that simultaneously accepts:
  - `fma.rn.bf16x2`
  - `cvt.rn.f32.bf16` / `cvt.rn.bf16.f32` / `cvt.rn.bf16x2.f32`
  - the first committed `mma.sync` subset on `sm_89`
- packed `bf16x2` `add` / `mul` and packed `bf16x2` `ex2.approx` require `sm_90+`
- `fma.rn.bf16x2` is accepted on `sm_89`
- `cvt.rn.f32.bf16`, `cvt.rn.bf16.f32`, and `cvt.rn.bf16x2.f32` are accepted on `sm_89`
- `mma.sync` rejects several input-material shapes when used as direct native shapes, including:
  - `m16n16k16`
  - `m8n8k16`
  - `m8n16k16`
  - `m16n16k8`
  - `m8n8k8`
- for the first committed `native-mma-sync` shapes, local probes accepted `row.col` and rejected `row.row`, `col.row`, and `col.col`
- local `nvdisasm` inspection on `sm_89` showed `fma.rn.bf16x2` lowering to `HFMA2.BF16_V2` and `cvt.rn.bf16.f32` lowering to `F2FP.BF16...`; this is treated as design evidence that the shared `sm_89` baseline retains a usable per-thread BF16 datapath subset

These probe notes are used here as design evidence. Reproducible implementation-time probes may later be added under `lab/` or another maintained location, but that is not a prerequisite for freezing this active baseline.

## Scope Rules for the Two Active Changes

### `support-custom-instructions`

This change must treat the following as already frozen by this document:

- `.version 7.8` / `sm_89` shared PTX baseline
- explicit oracle policy, with current non-MMA validation using Spike-backed OpenCL buffer comparison
- packed custom instruction contract
- shuffle / approximate SFU / `vcvt` shared semantic boundary

It must not silently redefine those shared decisions inside its own artifacts or implementation.

### `support-custom-mma`

This change must treat the following as already frozen by this document:

- `.version 7.8` / `sm_89` shared PTX baseline
- explicit oracle policy, with current MMA work still outside the Spike-backed surface and therefore still requiring an explicitly chosen non-Spike fallback until proven otherwise
- initial MMA commitment limited to the first `native-mma-sync` subset
- `wmma` reserved as active research rather than first-subset commitment

It must not silently expand first-phase scope to include `wmma` or composite MMA lowering without first updating this active baseline.

### Escalation rule

If either active change discovers that this shared baseline is insufficient or wrong, the required order is:

1. update this `active` document first
2. then update the affected change artifacts
3. and only then implement against the revised baseline

This prevents the two changes from drifting into incompatible hidden assumptions.

## Sync and Archive Conditions

This document should stay `active` only while the shared prerequisite decisions remain ahead of current implementation.

After the two active custom-instruction changes land and the resulting contracts are synced into `openspec/specs/`, this document should:

- either be archived into `doc/archive/`
- or be rewritten as a short `historical` record pointing readers to the synced current specs
