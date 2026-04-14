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

For every current supported MMA family, semantic validation MUST compare observable sbtsim PTX output, observable Spike output, and a repository-managed CPU reference under documented comparison rules.

- finite `fp16` lanes use `<= 1 fp16 ULP` or an equivalent documented host-side tolerance,
- finite `f32` lanes use documented `atol/rtol` tolerance against the same CPU reference,
- `NaN` results compare by classification rather than payload,
- and integer / structural metadata outputs remain exact-match only.

The current MMA oracle MUST exercise at least one small warp-multiple sample batch and one larger warp-multiple sample batch per supported family, using randomized finite input seeds rather than a single fixed deterministic batch only.

#### Scenario: Landed MMA kernel translates and passes the oracle
- **GIVEN** an input kernel uses only the landed current MMA subset
- **WHEN** `sbt_ptx --require-known --sm 89` translates it and the MMA oracle gate is executed
- **THEN** decode and PTX emission succeed
- **AND THEN** `ptxas -arch=sm_89` succeeds
- **AND THEN** the observable sbtsim PTX output and Spike output both match the CPU reference under the documented MMA comparison rules
- **AND THEN** the gate reports coverage for both a smaller random batch and a larger random batch

### Requirement: Unsupported or blocked MMA combinations still fail explicitly
The current landed MMA support MUST remain limited to the first-batch subset above.

Within the `fp16 -> fp16` family, only the following combinations are current supported behavior:

- direct-native `m16n8k16 row.col f16.f16.f16.f16`
- committed `split-n` composite `m16n16k16 row.col f16.f16.f16.f16`

Any other deferred/research MMA family, any non-`row.col` MMA family, and any other `fp16 -> fp16` combination MUST continue to fail explicitly under `--require-known`.

An unsupported `fp16 -> fp16` path MAY still decode as known through dedicated MMA metadata, but it MUST fail explicitly before PTX compile-first or semantic validation proceeds; the backend MUST NOT silently reinterpret it as one of the landed current families.

#### Scenario: Unsupported or blocked MMA path does not silently lower
- **GIVEN** an input kernel includes an MMA combination outside the landed current subset, or an `fp16 -> fp16` family outside the two current supported combinations
- **WHEN** `sbt_decode --require-known` or `sbt_ptx --require-known` is executed for a deferred/research/non-`row.col` family, or `sbt_ptx --require-known` enters lowering for a non-current `fp16 -> fp16` family
- **THEN** translation or lowering fails explicitly (`unknown` / `unsupported` / blocked diagnostic)
- **AND THEN** the current `inst-support` contract keeps the landed MMA subset and the remaining blocked/deferred/research MMA work clearly separated
