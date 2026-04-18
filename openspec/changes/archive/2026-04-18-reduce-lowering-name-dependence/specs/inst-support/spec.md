# inst-support (delta spec)

## MODIFIED Requirements

### Requirement: Current supported instructions SHALL provide explicit emit-authoritative descriptors before PTX emission
For the current supported instruction surface that may reach PTX emission, the repository MUST produce explicit structured descriptor or payload that is sufficient to drive lowering before `sbt/ptx_emit.cpp` selects a lowering path.

This requirement applies to:

- ordinary non-custom instructions on the current supported PTX path
- current supported structured control instructions whose PTX behavior is currently no-op, barrier, or return-like handling
- current supported custom non-MMA instruction families
- current supported MMA instruction families through their existing structured metadata path

The required emitter authority MAY be attached directly to `DecodedInst` or provided through repository-managed shared metadata consumed during decode, but it MUST be available before PTX emission begins.

For the current supported surface, PTX lowering MUST NOT reconstruct its semantic authority a second time from mnemonic text in the emitter.

#### Scenario: Supported ordinary instruction reaches PTX emission with explicit descriptor
- **GIVEN** an ordinary non-custom instruction belongs to the current supported PTX path
- **WHEN** PTX emission selects its lowering behavior
- **THEN** the required semantic authority is read from explicit decode-produced descriptor or shared metadata
- **AND THEN** the emitter does not determine that semantic authority by parsing `DecodedInst.name`

#### Scenario: Supported custom instruction reaches PTX emission with explicit payload
- **GIVEN** a current supported custom non-MMA instruction reaches PTX emission
- **WHEN** the emitter selects the lowering path
- **THEN** it uses explicit decoded payload such as `custom.family/subop/dtype` plus decoded operand fields
- **AND THEN** it does not reparse mnemonic stems, prefixes, or suffixes from `DecodedInst.name`

#### Scenario: Supported structured control instruction reaches PTX emission with explicit control descriptor
- **GIVEN** a current supported structured control instruction such as `setrpc`, `join`, `barrier`, `vsetvli`, or `endprg` reaches PTX emission
- **WHEN** the emitter selects its current supported no-op, barrier, or return-like behavior
- **THEN** it reads explicit control descriptor or equivalent structured metadata produced before emission
- **AND THEN** it does not choose that behavior by matching `DecodedInst.name` inside `sbt/ptx_emit.cpp`

#### Scenario: Missing emit-authoritative descriptor fails explicitly
- **GIVEN** a current supported instruction reaches PTX emission
- **AND GIVEN** its required descriptor or payload is missing, incomplete, or inconsistent with the supported contract
- **WHEN** the emitter attempts to lower the instruction
- **THEN** translation fails explicitly
- **AND THEN** the implementation does not silently recover by falling back to mnemonic parsing in `sbt/ptx_emit.cpp`

### Requirement: PTX emitter SHALL select supported lowering paths by descriptor domain and kind rather than mnemonic parsing
For the current supported PTX path, the emitter MUST organize lowering selection around explicit descriptor domain/kind or equivalent structured metadata, rather than around mnemonic-text dispatch.

This requirement covers at least:

- control-flow / call / return lowering
- scalar memory / scalar integer / scalar FP / CSR lowering
- vector memory / vector integer / vector FP / compare / mask / convert / move lowering
- custom non-MMA lowering
- MMA lowering through its existing structured metadata path

The implementation MAY still use `DecodedInst.name` for external-facing text, comments, diagnostics, or reporting, but not for supported-path semantic branch selection.

#### Scenario: Supported vector lowering selects by explicit vector descriptor
- **GIVEN** a current supported vector instruction reaches PTX emission
- **WHEN** the emitter selects vector arithmetic, compare, mask, convert, or memory behavior
- **THEN** it reads explicit vector-domain descriptor or metadata fields produced before emission
- **AND THEN** it does not use mnemonic-text branching as the authoritative semantic selector

## ADDED Requirements

### Requirement: DecodedInst.name SHALL remain an external mnemonic contract, not PTX emission authority
`DecodedInst.name` SHALL remain part of the current external decode contract for:

- pretty print
- JSON / diagnostics
- mnemonic coverage and reporting
- ABI-visible builtin symbol names where applicable

However, for the current supported PTX path, correctness of PTX emission MUST NOT depend on `DecodedInst.name` remaining the primary semantic key.

#### Scenario: Pretty print still exposes mnemonic names after emitter refactor
- **GIVEN** a decoded instruction belongs to the current supported PTX path
- **WHEN** `sbt_decode pretty` or JSON output is produced
- **THEN** the output still exposes the mnemonic through `DecodedInst.name`
- **AND THEN** correctness of PTX lowering no longer depends on that mnemonic string as emitter authority

#### Scenario: Coverage and reporting still observe the canonical mnemonic contract
- **GIVEN** a decoded instruction belongs to the current supported PTX path
- **WHEN** coverage, mnemonic reporting, or ABI-visible builtin-symbol reporting is produced
- **THEN** the output still uses the canonical external mnemonic contract
- **AND THEN** supported-path PTX lowering authority still comes only from explicit descriptor or payload fields
