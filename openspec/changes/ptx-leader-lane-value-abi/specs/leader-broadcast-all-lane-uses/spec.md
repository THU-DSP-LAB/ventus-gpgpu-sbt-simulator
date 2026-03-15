# leader-broadcast-all-lane-uses (delta spec)

## ADDED Requirements

### Requirement: All-lane consumers of scalar state SHALL read leader-produced values through explicit broadcast
When a lowering path needs all currently active lanes to observe the same Ventus scalar value, the emitter MUST obtain that value from the current leader lane through an explicit leader-to-all-lane broadcast step.

The implementation MUST NOT assume that all active lanes already hold the canonical scalar value locally.

#### Scenario: Vector-style use consumes a scalar source
- **GIVEN** a lowered PTX operation requires every active lane to consume the same Ventus scalar source
- **WHEN** the scalar source canonical value resides only in the current leader lane
- **THEN** the emitter first broadcasts that scalar source from the leader lane to all active lanes
- **AND THEN** the consuming all-lane operation uses the broadcast result

### Requirement: Scalar conditional branches SHALL broadcast comparison operands before `bra.uni`
Ventus scalar conditional branches (`beq`, `bne`, `blt`, `bge`, `bltu`, `bgeu`) MAY continue to lower to PTX `bra.uni`, but only after the lowering makes both comparison operands available to all active lanes via leader-to-all-lane broadcast.

The resulting branch predicate MUST be evaluated from the broadcast operands, not by directly reading non-leader local scalar registers.

#### Scenario: `beq` uses leader-broadcast operands
- **GIVEN** a translated `beq x5, x6, target`
- **AND GIVEN** canonical `x5` and `x6` values reside in the current leader lane
- **WHEN** the branch is lowered
- **THEN** the lowering first broadcasts `x5` and `x6` to the active subset
- **AND THEN** all active lanes evaluate the same comparison result before issuing `bra.uni`

### Requirement: Leader-only scalar execution SHALL remain the first implementation strategy
The first implementation of this change MUST keep scalar ALU, scalar memory, and other leader-only scalar side effects on the leader/leader-only execution path.

This capability MUST NOT require switching the general scalar execution model to “all lanes redundantly execute scalar instructions”.

#### Scenario: Scalar store remains leader-only despite branch fix
- **GIVEN** the new lowering path has already added leader-to-all-lane broadcast for scalar branch operands
- **WHEN** the program later executes a scalar store or atomic operation
- **THEN** the side-effecting scalar operation is still issued by the leader-only execution path
- **AND THEN** the branch broadcast fix does not imply that all scalar instructions now execute redundantly on all lanes

### Requirement: Broadcast use sites SHALL be defined explicitly in the design and validation plan
The design and implementation tasks for this capability MUST enumerate which lowering sites are all-lane consumers of Ventus scalar state and therefore require leader-to-all-lane broadcast.

At minimum, this list MUST include:
- scalar conditional branches
- existing `vmv_v_x` / `vmv_s_x` / `vfmv_v_f`-style paths
- existing `vmerge_vxm` / `vfmerge_vfm`-style paths
- existing `vx` arithmetic or equivalent mixed scalar-vector consumer paths

#### Scenario: Implementation does not stop at branch-only reasoning
- **GIVEN** a developer implements leader-broadcast handling for scalar conditional branches
- **WHEN** they review the remaining lowering paths
- **THEN** they can identify additional all-lane scalar consumers from the documented list instead of assuming branches are the only such use site

### Requirement: Current all-lane scalar consumers SHALL converge on explicit broadcast helpers rather than ad-hoc local reads
For the existing all-lane scalar-consumer inventory covered by this change, the implementation MUST route lowering paths through one or a small number of explicit broadcast helpers instead of allowing each use site to assume that every lane already holds the same scalar value locally.

This requirement exists to keep the consumer inventory auditable and to reduce the chance of branch-only fixes leaving stale local-read paths behind.

#### Scenario: New mixed scalar-vector lowering reuses the broadcast entry point
- **GIVEN** a new lowering path is identified as an all-lane consumer of Ventus scalar state
- **WHEN** that path is implemented under this change
- **THEN** it reuses the explicit broadcast entry point defined for all-lane scalar consumers
- **AND THEN** it does not introduce a fresh ad-hoc path that directly reads a non-leader local scalar value
