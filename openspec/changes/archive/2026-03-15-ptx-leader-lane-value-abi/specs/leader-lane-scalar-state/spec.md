# leader-lane-scalar-state (delta spec)

## ADDED Requirements

### Requirement: Function-body scalar canonical state SHALL reside in leader-lane PTX scalar registers
For the new PTX lowering path, the function-body canonical representation of Ventus `x-reg` state MUST be the PTX scalar register state held by exactly one current leader lane.

The emitter MUST treat:
- `leader_lane` as the identity of the canonical leader
- `x0` as hard-wired zero
- non-leader lanes as non-canonical carriers for scalar state

The emitter MUST NOT require per-warp shared `WarpCtx.x[]` to remain the canonical source of truth for this path.

#### Scenario: Scalar ALU chain stays in leader-lane PTX registers
- **GIVEN** a sequence of Ventus scalar ALU instructions that updates `x5`, then consumes `x5` again before any all-lane use
- **WHEN** the function is lowered through the leader-lane scalar-state path
- **THEN** the updated `x5` canonical value is carried by the current leader lane PTX scalar registers
- **AND THEN** correctness does not depend on reloading `x5` from per-warp shared `WarpCtx`

### Requirement: Each function activation SHALL establish a valid current leader before Ventus scalar execution
Each function activation under the leader-lane scalar-state path MUST establish one valid current leader inside the current active subset before any Ventus scalar-side lowering executes.

For the first implementation:
- kernel entry MAY choose any lane within the current active subset as the initial leader
- helper entry under the new value ABI MUST inherit the current leader from incoming call state rather than guessing a new one
- when the current leader has become invalid and a re-selection is required, the implementation MAY choose any lane within the current active subset; using `bfind(activemask)` as the first implementation's selection strategy is allowed

#### Scenario: Entry path chooses an initial active-subset leader
- **GIVEN** a kernel entry begins execution under the leader-lane scalar-state path
- **WHEN** the first Ventus scalar-side operation is about to execute
- **THEN** one current leader inside the current active subset has already been established
- **AND THEN** the implementation is allowed to use a simple active-subset selection strategy such as `bfind(activemask)` for that initial choice

### Requirement: Structured divergence SHALL legalize leader and scalar state at path entry
When execution enters a structured `vbranch -> join` region, the lowering MUST guarantee that every actually executed path begins with:
- a valid path-local leader that belongs to that path's active subset, and
- a valid path-entry scalar state equivalent to the branch-entry logical `x-state`

If one branch path has an empty active mask, the lowering MUST NOT require leader or scalar-state handoff for that non-executed path.

#### Scenario: Original leader is absent from one taken path
- **GIVEN** a `vbranch` where the pre-branch leader lane is active only in one path
- **AND GIVEN** the other path has a non-empty active mask
- **WHEN** the second path begins execution
- **THEN** that path already has a path-local leader inside its active subset
- **AND THEN** the path-local leader observes a scalar state equivalent to the branch-entry logical `x-state`

#### Scenario: Zero-mask path does not require state handoff
- **GIVEN** a `vbranch` where one branch result mask is empty
- **WHEN** the translated PTX follows the non-empty path only
- **THEN** the lowering does not require leader selection or scalar-state transfer for the empty path

### Requirement: Join-side reconvergence SHALL restore a single canonical scalar state without merge logic
At every structured `join`, the lowering MUST ensure that any scalar state remaining live after the `join` is reconverged to one logical warp-uniform `x-state`.

For the first implementation, the lowering MAY realize this by synchronizing path-final scalar state on each `join` predecessor edge, but the normative requirement is the post-`join` semantic result:
- reconverged active lanes observe one identical live scalar state, and
- exactly one current leader is chosen from the reconverged active subset

The `join` itself MUST NOT be specified as a semantic PHI/merge point for multiple divergent scalar states.

#### Scenario: Nested divergence reconverges to one current leader
- **GIVEN** an outer `vbranch -> join` region that contains an inner `vbranch -> join`
- **WHEN** the inner region completes and execution continues in the outer path
- **THEN** only one current leader and one reconverged logical scalar state remain live for the outer path
- **AND THEN** the lowering does not require restoring an older pre-inner leader identity

#### Scenario: Shared join reconverges stepwise without restoring historical leader identities
- **GIVEN** nested structured divergence causes the same `join` instruction to be reached multiple times as different path levels reconverge
- **WHEN** each effective predecessor edge reaches that shared `join`
- **THEN** the lowering applies predecessor-edge reconvergence to the currently completed path level only
- **AND THEN** each completed step leaves one current leader and one current reconverged scalar state rather than restoring a historical outer leader snapshot

### Requirement: The leader-lane scalar-state design SHALL depend on the compiler uniformity contract
This capability SHALL assume the accepted compiler contract for the current input domain:
- values that remain scalar (`x-reg` semantic) across `join` are already warp-uniform, and
- values that need lane-varying semantics across `join` are not kept as scalar live-out `x-reg` state

This change does NOT require the implementation to prove that post-`join` scalar uses satisfy that contract.

The contract remains primarily a compiler-side assumption for the current input domain. Implementations MAY add best-effort diagnostics for obviously unsupported shapes, but those diagnostics are optional and MUST NOT be treated as a complete legality proof.

#### Scenario: Live scalar value after join is expected to be uniform
- **GIVEN** a scalar value is read after a structured `join`
- **WHEN** that value remains represented as Ventus scalar state
- **THEN** the implementation is allowed to assume the value is already uniform across reconverged lanes
- **AND THEN** it does not need to perform a lane-wise semantic merge at the `join`
