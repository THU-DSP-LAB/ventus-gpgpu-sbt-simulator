# replicated-scalar-state (delta spec)

## MODIFIED Requirements

### Requirement: Scalar instructions SHALL be classified by execution semantics
The emitter MUST classify scalar-side instructions by execution semantics through an explicit repository-managed classification contract rather than by a simple side-effect-only rule, an emitter-local mnemonic helper, or an implicit default.

The lowering SHALL support at least these classes:

- `uniform-pure`
  - depends only on warp-uniform scalar inputs
  - has no externally observable side effect
  - may execute on all active lanes
- `lane-sensitive`
  - may depend on lane-sensitive state, vector-to-scalar extraction, or a single-lane observation model
  - must execute on one selected active leader lane
- `fixed-lane-sensitive`
  - reads scalarized data from one architecturally fixed source lane rather than an arbitrary selected leader
  - must preserve that fixed-lane architectural meaning explicitly
- `externally-side-effecting`
  - changes memory, atomic state, trap state, or other externally observable machine state
  - must execute on one selected active leader lane

For the current supported scalar subset, every scalar instruction that may reach PTX lowering and requires scalar execution classification MUST have an explicit classification.

Whether a current supported scalar instruction requires scalar execution classification at all MUST itself come from explicit metadata, descriptor domain, or an equivalent repository-managed tag, rather than from emitter-local mnemonic exemptions.

Instructions such as `vmv.x.s` MUST be treated as fixed-lane-sensitive, not as all-lane uniform-pure operations and not as arbitrary-leader operations.

If a scalar instruction that requires classification has no explicit execution-semantics classification, the implementation MUST fail explicitly rather than silently treating it as `uniform-pure`.

#### Scenario: Unclassified scalar instruction is rejected before name-based fallback
- **GIVEN** a new scalar instruction reaches PTX lowering on the current supported path
- **AND GIVEN** no explicit execution-semantics classification has been defined for it
- **WHEN** the emitter attempts to classify or lower the instruction
- **THEN** translation fails explicitly
- **AND THEN** the instruction is not lowered through an emitter-local name-based fallback that assumes `uniform-pure`

#### Scenario: Classification applicability is determined without mnemonic exemptions
- **GIVEN** a current supported scalar-side instruction reaches PTX lowering
- **WHEN** the implementation determines whether scalar execution classification is required for that instruction
- **THEN** the decision comes from explicit metadata, descriptor domain, or an equivalent repository-managed tag
- **AND THEN** the decision is not made through emitter-local mnemonic exceptions such as `jal`, `jalr`, or `vmv.x.s`

## ADDED Requirements

### Requirement: Current supported scalar-side PTX emission SHALL consume explicit scalar descriptors rather than mnemonic inference
For the current supported scalar-side PTX path, the guard and semantic selector that choose all-lane execution, leader-only execution, fixed-lane execution, branch condition semantics, scalar memory semantics, scalar integer semantics, scalar FP semantics, or CSR semantics MUST come from explicit decode-produced descriptors or repository-managed metadata rather than emitter-local mnemonic parsing.

This requirement applies to current supported scalar-side paths including at least:

- scalar conditional branches and related direct jump/call/return lowering guards
- scalar load/store families
- scalar integer and bitmanip families
- scalar FP families
- CSR families

This requirement does not force every scalar lowering helper to collapse into one monolithic table. It requires that the semantics used to choose the lowering mode, including whether scalar execution classification is applicable, are no longer re-inferred inside the emitter from `DecodedInst.name`.

#### Scenario: Scalar branch lowering reads explicit branch semantics
- **GIVEN** a current supported scalar conditional branch reaches PTX lowering
- **WHEN** PTX lowering selects the comparison semantics and branch form
- **THEN** the selection reads explicit branch-condition semantics from decode-produced descriptors or shared metadata
- **AND THEN** the emitter does not determine the semantic class by matching the mnemonic string

#### Scenario: Scalar load/store lowering reads explicit memory semantics
- **GIVEN** a current supported scalar load or store reaches PTX lowering
- **WHEN** PTX lowering selects sign-extension, raw-bit, or leader-only behavior
- **THEN** those semantics come from explicit descriptor or metadata fields produced before PTX emission
- **AND THEN** the lowering does not reconstruct the semantics by parsing `lw/lb/lh/lbu/lhu/sw/sb/sh/fsw/flw` names in the emitter

#### Scenario: Scalar FP lowering reads explicit floating-point semantics
- **GIVEN** a current supported scalar FP instruction reaches PTX lowering
- **WHEN** PTX lowering selects move/sign/minmax/compare/convert/class/fma or rounding-related behavior
- **THEN** the selection reads explicit scalar FP descriptor or metadata fields produced before PTX emission
- **AND THEN** the emitter does not use `fadd_s/fsgnj_s/fcvt_*` mnemonic matching as the authoritative semantic selector

#### Scenario: CSR lowering reads explicit CSR semantics
- **GIVEN** a current supported CSR instruction reaches PTX lowering
- **WHEN** the emitter selects the CSR lowering behavior
- **THEN** it reads explicit CSR descriptor or metadata fields produced before PTX emission
- **AND THEN** it does not use emitter-local mnemonic inference as the primary semantic authority
