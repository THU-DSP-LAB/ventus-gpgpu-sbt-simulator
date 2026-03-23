# replicated-scalar-state

> Status: `current`
>
> Canonical current PTX lowering contract for scalar-state, structured divergence, and direct-call preservation behavior.

## Purpose
Define the current PTX lowering contract for replicated active-lane scalar state, including scalar instruction classification, structured divergence behavior, direct-call state preservation, and the project assumptions behind all-lane scalar loads.

## Requirements

### Requirement: Live Ventus scalar state SHALL remain equal across current active lanes
For the replicated scalar-state PTX lowering path, any Ventus scalar state that remains live at a PTX execution point MUST have the same logical value on all currently active lanes.

This requirement applies to:

- scalar general-register state represented as Ventus `xreg`
- scalar CSR values represented in the same replicated scalar-state domain

This requirement does not apply to inactive lanes.

#### Scenario: Uniform scalar ALU chain preserves active-lane equality
- **GIVEN** a sequence of scalar ALU instructions updates `x5` and `x6`
- **WHEN** those instructions are lowered through the replicated scalar-state path
- **THEN** every currently active lane observes the same logical values for live `x5` and `x6`

#### Scenario: Dead divergent scalar remnants do not affect post-join live state
- **GIVEN** two divergent paths leave different values in a scalar register that is dead before `join`
- **WHEN** execution reconverges after the `join`
- **THEN** correctness does not depend on preserving those dead path-local remnants
- **AND THEN** only scalar state that remains live after reconvergence must satisfy active-lane equality

### Requirement: Scalar instructions SHALL be classified by execution semantics
The emitter MUST classify scalar-side instructions by execution semantics rather than by a simple side-effect-only rule.

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

Instructions such as `vmv.x.s` MUST be treated as fixed-lane-sensitive, not as all-lane uniform-pure operations and not as arbitrary-leader operations.

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

### Requirement: Divergent paths SHALL select a valid path-local leader only when needed
The replicated scalar-state lowering MUST NOT require a permanent leader-lane ownership model for all scalar execution.

When a divergent path encounters a leader-only scalar instruction, the lowering MUST ensure that:

- one valid leader is selected from that path's current active mask
- the selected leader belongs to the currently executing path

If a divergent path executes only all-lane scalar instructions, it MUST NOT need leader reselection solely to maintain scalar-state protocol.

When a leader-only instruction writes replicated scalar state, the lowering MUST restore the replicated active-lane equality invariant for every affected live scalar destination before any subsequent PTX operation that may observe replicated scalar state.

#### Scenario: Divergent path without leader-only operations does not force leader handoff
- **GIVEN** a `vbranch` creates two non-empty paths
- **AND GIVEN** one path executes only uniform-pure scalar instructions before reconvergence
- **WHEN** that path is lowered
- **THEN** the path does not need a path-entry full-`x` broadcast or unconditional leader reselection

#### Scenario: Divergent path with leader-only operation selects a local leader lazily
- **GIVEN** a divergent path where the pre-branch leader is not active
- **AND GIVEN** the path later executes a scalar store
- **WHEN** the leader-only store is emitted
- **THEN** the path first selects a valid leader from its current active mask
- **AND THEN** the store is executed exactly once by that selected leader

#### Scenario: Leader-only scalar write re-replicates before later scalar use
- **GIVEN** a divergent path executes a leader-only scalar write that updates a live Ventus scalar destination
- **AND GIVEN** a later scalar ALU instruction, scalar branch, call-state marshal, or reconvergence step reads replicated scalar state
- **WHEN** the later replicated-state consumer executes
- **THEN** the updated scalar destination has already been re-replicated across the current active lanes
- **AND THEN** stale non-leader copies are not allowed to remain observable

### Requirement: Structured reconvergence SHALL NOT depend on full-`x` broadcast
The PTX lowering for `vbranch/join` MUST NOT use full-`x` broadcast as the default correctness protocol.

At reconvergence, the lowering MAY synchronize only the scalar results that are required by leader-only instructions or subsequent live scalar uses, but it MUST NOT treat the entire `xreg` file as a mandatory control-flow payload.

The implementation MAY rely on the current Ventus compiler contract that any scalar state still live after `join` is uniform on active lanes.

#### Scenario: Join does not materialize the full scalar register file
- **GIVEN** a `join` after divergent paths in which only a small live scalar subset remains relevant
- **WHEN** reconvergence code is emitted
- **THEN** correctness does not require shuffling every `xreg`
- **AND THEN** scalar state outside the required live subset is not made observable solely because a `join` occurs

### Requirement: Direct-call lowering SHALL preserve machine-state semantics without eager full-state materialization
PTX direct-call lowering MUST continue to preserve the required Ventus machine-state semantics across `.func` call and return boundaries.

However, the lowering MUST NOT assume that every scalar state slot needs to become explicit PTX live state before and after each call boundary.

The design MAY keep `value_blob` as the PTX call shape, but it SHALL avoid eager full-state materialization that turns ABI-only dead state into observable PTX dataflow by default.

#### Scenario: ABI-only dead call state remains non-observable
- **GIVEN** a call boundary where some machine-state fields are present in the ABI shape but are not used by the callee and are not observed by the caller afterward
- **WHEN** direct-call lowering is emitted
- **THEN** the design does not require those fields to become explicit long-lived PTX register state merely because they cross the call boundary

#### Scenario: Live call state still remains semantically preserved
- **GIVEN** a call boundary where a subset of scalar and vector machine state is semantically observed after return
- **WHEN** direct-call lowering is emitted
- **THEN** that observed subset remains correctly preserved across the PTX `.func` call and return boundary

### Requirement: Scalar loads MAY execute on all active lanes under the current project assumptions
Ordinary scalar loads may be lowered as all-lane operations when all of the following current project assumptions hold:

- no MMIO-like load semantics are required
- no special PTX memory proxy or non-coherent load path is used
- program correctness does not rely on unsynchronized same-address read/write observation behavior

Under those assumptions, repeating the same ordinary load on all active lanes is an allowed implementation choice.

#### Scenario: Uniform scalar load reads on all active lanes
- **GIVEN** a scalar `lw` from a uniform address in the supported current input domain
- **WHEN** the load is lowered through the replicated scalar-state path
- **THEN** the PTX lowering may issue the load on all active lanes
- **AND THEN** the loaded scalar destination remains equal across active lanes
