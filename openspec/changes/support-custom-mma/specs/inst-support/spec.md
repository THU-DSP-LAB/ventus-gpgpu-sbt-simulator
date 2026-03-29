# inst-support (delta spec)

## ADDED Requirements

### Requirement: MMA support uses an explicit support matrix
This change MUST treat MMA as a separate capability set with an explicit support matrix covering:

- Ventus `shape`
- `alayout` / `blayout`
- A/B input type
- C/D accumulator/output type
- required PTX target
- chosen lowering path (`native mma.sync`, decomposed sequence, or explicit unsupported)

The project MUST NOT assume that every Ventus MMA combination from `doc/CUSTOM_INSTRUCTION_INPUT.md` is natively expressible as a single PTX MMA instruction.

The support matrix MUST be produced in a way that is consistent with the shared active baseline document, rather than assuming that this change may choose its own PTX baseline or first-subset scope independently.

For the first committed `native-mma-sync` subset, the active artifacts MUST explicitly name the supported layout pairs rather than leaving them implicit under a shape-only entry.

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

### Requirement: MMA implementation waits for the shared active baseline
Before the project implements supported MMA combinations, the active custom-instruction changes MUST freeze a shared active baseline that decides a single project-wide PTX baseline and the MMA first-subset boundary.

#### Scenario: MMA implementation does not race ahead of baseline planning
- **GIVEN** the MMA support matrix identifies combinations with differing PTX target requirements
- **WHEN** the project is preparing to implement the supported subset
- **THEN** MMA implementation waits until the shared active baseline has decided the current project-wide PTX baseline and first-subset boundary
- **AND THEN** the project does not let MMA retroactively force a second hidden baseline bump after non-MMA implementation has already landed

### Requirement: MMA semantic validation covers the supported subset
The project MUST provide focused validation for each MMA combination marked supported by the matrix.

The validation MUST make the instruction effect observable, and MUST compare outputs against an explicitly chosen oracle path rather than relying on compile-first success alone.

#### Scenario: Supported MMA combination is semantically checked
- **GIVEN** an MMA combination is marked supported by the matrix
- **WHEN** the corresponding validation test is executed
- **THEN** the output reflects the MMA result through observable buffers or equivalent externally visible state
- **AND THEN** mismatches fail explicitly
