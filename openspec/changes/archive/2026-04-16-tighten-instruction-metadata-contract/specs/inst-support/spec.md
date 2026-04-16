# inst-support (delta spec)

## ADDED Requirements

### Requirement: Spike-backed instruction metadata SHALL be repository-managed and shared across decode and CFG analysis
For current supported non-custom instructions recognized through the Spike-backed pattern subset, the repository MUST maintain structured instruction metadata that is authoritative for at least:

- operand form
- immediate form
- vector uniform-transfer behavior needed by CFG verification

Decode and CFG verification MUST consume the same repository-managed metadata rather than each inferring behavior independently from mnemonic suffixes such as `_vx`, `_vi`, `_vv`, or `_v`.

Dedicated repository-local metadata for custom non-MMA and MMA instruction families MAY remain separate, but Spike-backed non-custom instructions MUST still follow the same “explicit metadata, single source of truth” principle.

#### Scenario: New Spike-backed vector instruction updates one shared metadata entry and remains reachable
- **GIVEN** a developer adds support for a Spike-backed non-custom instruction whose operand shape or uniform-transfer behavior does not fit a simple suffix rule
- **WHEN** they update the repository-managed instruction metadata for that mnemonic
- **AND WHEN** they also update the repository-managed Spike want list and rebuild the embedded Spike pattern subset as required by the current build-time subset contract
- **THEN** decode and CFG verification both observe the new operand and analysis behavior from the same metadata source
- **AND THEN** the change does not require maintaining parallel suffix-inference tables in multiple pipeline stages
- **AND THEN** the metadata entry is reachable through the same build-time Spike-backed decode entrypoint that current `sbt_decode` and `sbt_ptx` use

#### Scenario: Missing shared metadata does not silently fall back to suffix guessing
- **GIVEN** a Spike-backed non-custom instruction is treated as part of the current supported subset
- **AND GIVEN** its required repository-managed metadata entry is missing or incomplete
- **WHEN** the instruction reaches decode or CFG verification on the supported path
- **THEN** the implementation fails explicitly
- **AND THEN** the instruction is not accepted by silently guessing operand or analysis semantics from its mnemonic suffix alone
