# inst-support (delta spec)

## ADDED Requirements

### Requirement: MMA support uses an explicit support matrix
This change MUST treat MMA as a separate capability set with an explicit support matrix covering:

- Ventus `shape`
- `alayout` / `blayout`
- A/B input type
- C/D accumulator/output type
- required PTX target
- chosen lowering path (`native mma.sync`, committed `split-n` composite, research-only `wmma`, or explicit unsupported)

The project MUST NOT assume that every Ventus MMA combination from `doc/CUSTOM_INSTRUCTION_INPUT.md` is natively expressible as a single PTX MMA instruction.

The support matrix MUST be produced in a way that is consistent with the shared active baseline document, rather than assuming that this change may choose its own PTX baseline or first-batch scope independently.

For the first committed MMA batch, the active artifacts MUST explicitly name the supported layout pairs and lowering classes rather than leaving them implicit under a shape-only entry.

The canonical master matrix MUST live in `doc/mma/LOWERING_ARCHITECTURE.md`. It MUST list only the committed first-batch entries. Frontend-exposed but uncommitted combinations MUST be tracked separately as inventory and MUST NOT be presented as if they already had a proven lowering path.

#### Scenario: Unsupported MMA combination fails explicitly
- **GIVEN** an input kernel uses an MMA combination outside the implemented support matrix
- **WHEN** `sbt_ptx --require-known` translates the kernel
- **THEN** translation fails fast with an explicit diagnostic naming the unsupported shape/layout/type combination
- **AND THEN** the backend does not silently substitute a different MMA form

### Requirement: Supported MMA combinations are compile-supported and documented
For any MMA combination marked supported by the support matrix, the project MUST:

- decode the instruction as known,
- lower it through the documented PTX path,
- compile the emitted PTX with `ptxas` for the required target,
- and document the supported subset in current docs once implemented.

#### Scenario: Supported MMA path compiles
- **GIVEN** an input kernel uses only MMA combinations marked supported by the matrix
- **WHEN** the kernel is translated under the required target settings
- **THEN** PTX emission succeeds
- **AND THEN** `ptxas` compilation succeeds without requiring undocumented flags or fallback paths

### Requirement: First-batch MMA support is limited to the committed `row.col` families
The first committed MMA batch MUST stay within the `row.col` families frozen by the shared active baseline:

- direct-native `m16n8k16` / `m16n8k8`
- committed `split-n` composite `m16n16k16` / `m16n16k8`

Other layouts, `m8*` shapes, and other composite forms MUST remain outside the first commitment unless the shared active baseline is revised first.

#### Scenario: Deferred MMA family fails fast
- **GIVEN** an input kernel uses an MMA combination outside the committed first-batch families
- **WHEN** `sbt_ptx --require-known` translates the kernel
- **THEN** translation fails fast with an explicit unsupported diagnostic
- **AND THEN** the backend does not silently reinterpret the combination as one of the committed first-batch families

### Requirement: MMA implementation waits for the shared active baseline
Before the project implements supported MMA combinations, the active custom-instruction changes MUST freeze a shared active baseline that decides a single project-wide PTX baseline and the MMA first-batch boundary.

#### Scenario: MMA implementation does not race ahead of baseline planning
- **GIVEN** the MMA support matrix identifies combinations with differing PTX target requirements
- **WHEN** the project is preparing to implement the supported subset
- **THEN** MMA implementation waits until the shared active baseline has decided the current project-wide PTX baseline and first-batch boundary
- **AND THEN** the project does not let MMA retroactively force a second hidden baseline bump after non-MMA implementation has already landed

### Requirement: MMA semantic validation covers the supported subset
The project MUST provide focused validation for each MMA combination marked supported by the matrix.

The validation MUST make the instruction effect observable, and MUST compare outputs against the Spike-backed canonical oracle path rather than relying on compile-first success alone.

Repository-local helper models may still be used as bring-up cross-checks, but they do not replace the canonical semantic contract for the supported subset.

#### Scenario: Supported MMA path is checked against the Spike-backed oracle
- **GIVEN** an MMA combination is marked supported by the matrix
- **AND GIVEN** the repository provides a validation kernel that makes the MMA result observable through output buffers or equivalent external state
- **WHEN** the MMA semantic validation gate is executed
- **THEN** the gate runs the same observable test through the Spike-backed path and the PTX path
- **AND THEN** it compares the resulting outputs explicitly rather than treating compile-first success as sufficient evidence of semantic correctness

### Requirement: First-batch MMA mapping rules are formal and Spike-equivalent
For each first-batch committed MMA family, the project MUST freeze formal mapping rules that include:

- VGPR window summary,
- VGPR window / lane-slot -> logical tile mapping,
- and logical tile -> PTX fragment tuple construction rules.

The canonical source of truth for the Ventus-side mapping is Spike's execution semantics (the `load_matrix_*` / `store_matrix_d` index rules). The project MUST NOT rely on compile-first success as evidence that fragment ordering is correct.

#### Scenario: MMA mapping is not guessed
- **GIVEN** an MMA family is part of the first committed batch
- **WHEN** the project implements PTX lowering for that family
- **THEN** the lowering uses the frozen formal mapping rules rather than an ad-hoc tuple order
- **AND THEN** PTX fragment ABI instantiation comes from explicit native ABI descriptors sourced from the PTX operand contract rather than from raw VGPR window order alone

### Requirement: MMA decode metadata is first-class and separate from non-MMA custom metadata
The project MUST treat MMA decode metadata as a first-class structured object rather than overloading the existing non-MMA `CustomInstInfo` fields.

For supported and deferred MMA combinations alike:

- `DecodedInst.custom.family` MUST identify the instruction as `CustomFamily::Mma`
- shape/layout/type/window information MUST come from a dedicated `MmaInstInfo`
- the lowering path MUST consume `MmaInstInfo` rather than re-parsing raw instruction bits in the emitter

#### Scenario: MMA decode does not pollute non-MMA metadata
- **GIVEN** the repository-local decoder recognizes an MMA instruction
- **WHEN** a later lowering stage queries its metadata
- **THEN** the stage reads shape/layout/type/window information from dedicated MMA metadata
- **AND THEN** the existing non-MMA `CustomSubOp` / `CustomDataType` fields are not repurposed as MMA shape carriers

#### Scenario: Supported MMA combination is semantically checked
- **GIVEN** an MMA combination is marked supported by the matrix
- **WHEN** the corresponding validation test is executed
- **THEN** the output reflects the MMA result through observable buffers or equivalent externally visible state
- **AND THEN** mismatches fail explicitly
