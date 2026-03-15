# ptx-emitter-leader-derivation (delta spec)

## MODIFIED Requirements

### Requirement: PTX emitter SHALL derive active mask and leader predicate at lowering use points
The PTX emitter MUST treat:
- `leader_lane` as the only persistent leader metadata
- `activemask` as a derived execution-state query
- leader predicate as a derived comparison `laneid == leader_lane`

The emitter MUST NOT require every basic block entry to materialize and cache a fixed `%r1/%r2/%p0` leader context bundle.

#### Scenario: Pure vector block does not pay leader preamble tax
- **GIVEN** a basic block contains only vector arithmetic and control flow that does not require leader-only scalar execution
- **WHEN** the PTX emitter lowers that block
- **THEN** the block does not need an unconditional `activemask/bfind/setp` preamble solely to reconstruct legacy leader context

### Requirement: Leader predicate SHALL be derived from `leader_lane`, not from `bfind(activemask)`
Any leader-only lowering path MUST derive its leader predicate from the current `leader_lane`.

The emitter MUST NOT use “first active lane of the current mask” as a semantic substitute for the leader of canonical scalar state.

#### Scenario: Divergence path leader differs from first active lane
- **GIVEN** a divergence path whose current leader is not the first active lane in the current mask
- **WHEN** an leader-only scalar operation is lowered
- **THEN** the operation executes under the predicate derived from `leader_lane`
- **AND THEN** correctness does not depend on `bfind(activemask)`

### Requirement: Call and divergence boundaries SHALL invalidate cached derived leader state
Whenever control crosses a boundary that can change the current execution subset or returned leader identity, previously derived leader predicates and active-mask snapshots MUST be treated as stale.

This applies at least to:
- direct-call return
- structured divergence entry
- structured join reconvergence

#### Scenario: Call return recomputes leader-only predicate
- **GIVEN** a helper call may return a current leader identity for the resumed path
- **WHEN** the caller resumes lowering leader-only scalar code after the call
- **THEN** it derives leader predicate again from the post-call leader identity
- **AND THEN** it does not keep using a pre-call cached leader predicate

### Requirement: Migration SHALL preserve correctness of any still-live legacy shared-state path
If the implementation temporarily keeps legacy shared-state lowering paths during migration, the emitter MUST preserve any synchronization still required by those legacy paths for correctness.

The removal of unconditional leader preambles MUST NOT be used as justification to delete synchronization from unrelated legacy shared-state protocols that still need it.

#### Scenario: Legacy shared scalar write path remains correct during migration
- **GIVEN** some lowering path still writes shared scalar backing as part of a staged migration
- **WHEN** block-entry leader preambles are removed from the new leader-lane path
- **THEN** any synchronization still required by the surviving legacy shared path remains in place until that path is retired
