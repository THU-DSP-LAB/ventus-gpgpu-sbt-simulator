# replicated-scalar-state (delta spec)

## MODIFIED Requirements

### Requirement: Scalar instructions SHALL be classified by execution semantics
The emitter MUST classify scalar-side instructions by execution semantics through an explicit repository-managed classification contract rather than by a simple side-effect-only rule or by an implicit default.

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
- `externally side-effecting`
  - changes memory, atomic state, trap state, or other externally observable machine state
  - must execute on one selected active leader lane

For the current supported scalar subset, every scalar instruction that may reach PTX lowering MUST have an explicit execution-semantics classification.

Instructions such as `vmv.x.s` MUST be treated as fixed-lane-sensitive, not as all-lane uniform-pure operations and not as arbitrary-leader operations.

If a scalar instruction has no explicit execution-semantics classification, the implementation MUST fail explicitly rather than silently treating it as `uniform-pure`.

#### Scenario: Uniform-pure scalar instruction executes on all active lanes
- **GIVEN** a scalar `add` whose operands are live replicated scalar registers
- **WHEN** the instruction is emitted
- **THEN** the PTX lowering may execute the operation on all active lanes directly
- **AND THEN** the destination scalar state remains equal across active lanes

#### Scenario: Fixed-lane scalarization does not run on all lanes
- **GIVEN** a `vmv.x.s` instruction that extracts scalar state from vector state
- **WHEN** the instruction is emitted
- **THEN** the PTX lowering preserves the architectural source-lane meaning of the instruction rather than substituting an arbitrary selected leader
- **AND THEN** if the required fixed source lane is not active and the current supported input contract does not permit that case, the implementation rejects the shape explicitly rather than silently changing semantics

#### Scenario: Unclassified scalar instruction is rejected explicitly
- **GIVEN** a new scalar instruction reaches PTX lowering on the supported path
- **AND GIVEN** no explicit execution-semantics classification has been defined for that instruction
- **WHEN** the emitter attempts to classify it
- **THEN** translation fails explicitly
- **AND THEN** the instruction is not lowered as `uniform-pure` by default
