# inst-support (delta spec)

## MODIFIED Requirements

### Requirement: Current contract supports the landed first-batch MMA subset on `sm_89`
The current landed custom instruction support surface MUST include the committed `row.col` MMA subset below on the shared `.version 7.8` / `.target sm_89` baseline:

- direct-native:
  - `m16n8k16 row.col f16.f16.f16.f16`
  - `m16n8k16 row.col f32.f16.f16.f32`
  - `m16n8k16 row.col f32.bf16.bf16.f32`
  - `m16n8k8 row.col f32.tf32.tf32.f32`
- committed `split-n` composite:
  - `m16n16k16 row.col f16.f16.f16.f16`
  - `m16n16k16 row.col f32.f16.f16.f32`
  - `m16n16k16 row.col f32.bf16.bf16.f32`
  - `m16n16k8 row.col f32.tf32.tf32.f32`

For this landed subset, the project MUST:

- decode the MMA instruction as known through dedicated MMA metadata,
- emit PTX through the documented native/composite lowering path,
- pass compile-first validation with `ptxas -arch=sm_89`,
- and pass semantic validation using observable outputs.

For the current supported `fp16 -> fp16` families, semantic validation MUST compare observable PTX and Spike outputs against a CPU reference with an explicit fp16-tolerant comparison rule:

- finite results use `<= 1 fp16 ULP` or an equivalent documented host-side tolerance,
- `NaN` results compare by classification rather than payload,
- and integer / structural metadata outputs remain exact-match only.

#### Scenario: Landed MMA kernel translates and passes the oracle
- **GIVEN** an input kernel uses only the landed current MMA subset
- **WHEN** `sbt_ptx --require-known --sm 89` translates it and the MMA oracle gate is executed
- **THEN** decode and PTX emission succeed
- **AND THEN** `ptxas -arch=sm_89` succeeds
- **AND THEN** the observable Spike and PTX outputs match the CPU reference under the documented MMA comparison rules

### Requirement: Unsupported or non-current MMA combinations still fail explicitly
The current landed MMA support MUST remain limited to the subset above.

Within the `fp16 -> fp16` family, only the following combinations are current supported behavior:

- direct-native `m16n8k16 row.col f16.f16.f16.f16`
- committed `split-n` composite `m16n16k16 row.col f16.f16.f16.f16`

Any other deferred/research MMA family, any non-`row.col` MMA family, and any other `fp16 -> fp16` combination MUST continue to fail explicitly under `--require-known`.

An unsupported `fp16 -> fp16` path MAY still decode as known through dedicated MMA metadata, but it MUST fail explicitly before PTX compile-first or semantic validation proceeds; the backend MUST NOT silently reinterpret it as one of the landed current families.

#### Scenario: Unsupported or non-current MMA path does not silently lower
- **GIVEN** an input kernel includes an MMA combination outside the landed current subset, or an `fp16 -> fp16` family outside the two current supported combinations
- **WHEN** `sbt_decode --require-known` or `sbt_ptx --require-known` is executed for a deferred/research/non-`row.col` family, or `sbt_ptx --require-known` enters lowering for a non-current `fp16 -> fp16` family
- **THEN** translation or lowering fails explicitly (`unknown` / `unsupported` / blocked diagnostic)
- **AND THEN** the current `inst-support` contract keeps the landed MMA subset and the remaining blocked/deferred/research MMA work clearly separated

## ADDED Requirements

### Requirement: Current fp16-to-fp16 native MMA lowering uses an explicitly validated direct window-to-tuple contract
For `m16n8k16 row.col f16.f16.f16.f16`, the PTX backend MUST instantiate native operand tuples through an explicit repository-local contract that is validated against:

- Spike-equivalent `VGPR window -> logical tile` semantics,
- a CPU reference,
- and a handwritten-PTX probe for the same native PTX form.

The implementation MUST remain consistent with the layered model in `doc/mma/LOWERING_ARCHITECTURE.md` at the semantic level:

- `VGPR window`
- `logical coordinates`
- `PTX native fragment tuple`

The implementation MUST NOT require a materialized logical-tile buffer as an intermediate result. Instead, it MAY construct PTX tuples directly from the Ventus source windows, provided that the tuple element coordinates are derived from an explicit Spike-equivalent logical-coordinate contract rather than guessed from raw VGPR window order.

#### Scenario: Supported native fp16-to-fp16 path does not guess tuple order
- **GIVEN** `m16n8k16 row.col f16.f16.f16.f16` is lowered on the current `sm_89` baseline
- **WHEN** the emitter constructs PTX operand tuples
- **THEN** it uses an explicit validated direct window-to-tuple contract derived from the repository-local MMA ABI descriptors
- **AND THEN** the lowering does not guess PTX tuple order directly from raw Ventus register-window order alone

### Requirement: Current fp16-to-fp16 split-n lowering reuses the validated native building block
For `m16n16k16 row.col f16.f16.f16.f16`, the PTX backend MUST lower one Ventus instruction into exactly two native `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16` sub-operations over disjoint logical `n` slices.

The composite contract is:

- sub-op 0 covers logical `n = [0, 7]`,
- sub-op 1 covers logical `n = [8, 15]`,
- both sub-ops share the same logical `A` coordinate space,
- and `B/C/D` tuple construction and merge steps follow the same explicit ABI-descriptor contract as the native `m16n8k16` family.

#### Scenario: Supported split-n fp16-to-fp16 path rebuilds the full logical tile
- **GIVEN** `m16n16k16 row.col f16.f16.f16.f16` is lowered on the current `sm_89` baseline
- **WHEN** the PTX backend emits the composite lowering
- **THEN** it emits exactly two native `m16n8k16 row.col f16/f16/f16/f16` sub-operations
- **AND THEN** the combined writeback reconstructs the original logical `16 x 16` output tile without overlap or silent truncation
