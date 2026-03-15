# direct-call-value-abi (delta spec)

## ADDED Requirements

### Requirement: Direct call SHALL use a unified value ABI for mutable Ventus state
The PTX lowering path for non-inlined direct calls MUST exchange mutable Ventus execution state through an explicit value-style call ABI rather than the current `.local vctx` context-spill model.

The mutable call state MUST include at least:
- complete logical `x-reg` state
- complete logical `v-reg` state
- mutable program-observable CSR state required by the backend
- `leader_lane`

#### Scenario: Helper call no longer depends on `.local vctx` as the primary state carrier
- **GIVEN** a translated kernel contains a direct call to an emitted helper `.func`
- **WHEN** the new call ABI path is used
- **THEN** caller/callee state exchange is defined in terms of explicit value-state inputs/outputs
- **AND THEN** correctness does not depend on caller/callee sharing `%v0..%v255` through a `.local vctx` pointer

### Requirement: Value ABI SHALL separate mutable state, read-only machine context, and runtime environment
The direct-call ABI MUST distinguish three state classes:
- mutable call state
- read-only machine context
- read-only runtime environment

Read-only runtime environment fields MUST NOT be documented as Ventus architectural machine state.

Read-only machine context MAY be materialized either as explicit call-ABI fields or as recomputable values, but the design MUST define which fields belong to this layer.

#### Scenario: Runtime backing pointers remain distinct from Ventus machine state
- **GIVEN** a helper call requires access to ELF/global backing and heap/global backing
- **WHEN** the call ABI is specified
- **THEN** those backing pointers are modeled as runtime-environment inputs
- **AND THEN** they are not described as mutable Ventus machine state

### Requirement: Implicit execution state SHALL NOT be serialized into the value ABI
The direct-call value ABI MUST NOT serialize execution-state fields whose authoritative source remains the PTX / hardware execution model.

At minimum, this exclusion applies to:
- `active mask`
- PTX / hardware-managed reconvergence context
- any convenience field that duplicates the current executing subset or its derived state

These fields MUST be derived from the live execution state when needed, rather than carried as mutable call-state payload.

#### Scenario: Active mask stays outside the value ABI
- **GIVEN** a helper call executes under the new value ABI
- **WHEN** the caller/callee parameter shape is defined
- **THEN** the ABI does not include an explicit serialized `active mask` field
- **AND THEN** any lowering that needs the current active subset obtains it from the live execution state instead of from call-state payload

### Requirement: Direct call SHALL preserve leader-lane scalar semantics across call and return
For the leader-lane scalar-state path, direct call lowering MUST preserve the logical leader-lane scalar semantics across call and return.

Specifically:
- caller-to-callee state transfer MUST make the caller-visible scalar state available to the callee under the same logical leader identity for that executing path
- return-state transfer MUST restore the callee-produced scalar state to the caller under the returned leader identity
- call/return lowering MUST NOT require non-leader lanes to become canonical carriers of `x-reg` state
- scalar-state marshalling MAY remain leader-authoritative rather than first broadcasting scalar state to all active lanes
- the callee MUST restore `leader_lane` and canonical scalar state from the incoming call-state payload before executing any Ventus scalar-side operation
- the caller MUST treat the returned `leader_lane` and returned scalar state as authoritative before resuming post-call Ventus scalar lowering

#### Scenario: Helper modifies scalar state and returns to caller
- **GIVEN** caller path leader lane holds canonical scalar state before a helper call
- **AND GIVEN** the helper updates one or more scalar registers and returns
- **WHEN** caller resumes after the call
- **THEN** the resumed path observes the helper-updated logical scalar state
- **AND THEN** that scalar state is still interpreted through the current returned leader identity rather than through shared `WarpCtx` replay

#### Scenario: Callee restores leader metadata before scalar work
- **GIVEN** a helper is entered through the new direct-call value ABI
- **WHEN** the callee begins executing
- **THEN** it restores `leader_lane` and canonical scalar state from the incoming call-state payload before any Ventus scalar-side operation executes
- **AND THEN** the callee does not perform leader-only Ventus scalar initialization against an uninitialized or guessed leader identity

### Requirement: Divergence-local direct call SHALL use the current path-local call state
When a direct call executes inside a structured divergence path, the callee MUST consume the current path-local mutable call state for that path.

This includes the path-local:
- scalar state
- leader identity
- vector state
- mutable CSR state

#### Scenario: Direct call inside one divergence path does not reuse an obsolete outer leader
- **GIVEN** execution is inside a structured divergence path with a path-local leader
- **WHEN** that path performs a direct helper call
- **THEN** the helper receives the path-local leader and path-local mutable state for that path
- **AND THEN** the call does not reinterpret scalar ownership using a stale pre-divergence leader

### Requirement: The first implementation MAY use a conservative physical blob layout without freezing it as long-term ABI shape
The first implementation MAY realize the value ABI through one or more aggregate blob-like parameter objects chosen for correctness and maintainability.

However, the normative contract is the logical state layering and caller/callee semantics, not one permanently frozen physical packing strategy.

#### Scenario: Future layout refinement preserves logical ABI semantics
- **GIVEN** a later implementation splits a single mutable-state blob into several aggregate objects
- **WHEN** the logical caller/callee mutable-state contract remains unchanged
- **THEN** the change is treated as a physical-layout refinement rather than a semantic ABI redesign
