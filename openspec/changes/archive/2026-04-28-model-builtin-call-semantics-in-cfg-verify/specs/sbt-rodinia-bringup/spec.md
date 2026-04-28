# sbt-rodinia-bringup Delta

## ADDED Requirements

### Requirement: CFG verification SHALL model inlined builtin helper call semantics

The CFG verifier SHALL apply verifier-visible summaries for helper calls that the current PTX emitter later inlines from direct-call sites.

The summary model MUST cover every helper name accepted by the current builtin inline lookup. For each covered helper, the model MUST state the vector registers relevant to vector-uniform analysis that are written by the helper and the uniformity transfer rule for each written register.

The verifier MUST NOT treat a builtin call as an opaque `jal` whose returned vector value inherits the caller's pre-call vector-uniform fact.

Scalar register effects are outside this requirement unless a later verifier proof depends on them.

#### Scenario: `get_local_id(0)` makes `%v0` lane-varying
- **GIVEN** a function initializes `%v0` uniformly with dimension `0`
- **AND GIVEN** the function calls `_Z12get_local_idj`
- **WHEN** CFG verification computes vector-uniform facts after the call
- **THEN** `%v0` is treated as work-item-varying
- **AND THEN** a later `vbranch` using `%v0` is not proven uniform solely from the pre-call `%v0` state

#### Scenario: `get_group_id(0)` keeps `%v0` work-group-uniform
- **GIVEN** a function initializes `%v0` uniformly with dimension `0`
- **AND GIVEN** the function calls `_Z12get_group_idj`
- **WHEN** CFG verification computes vector-uniform facts after the call
- **THEN** `%v0` is treated as work-group-uniform

#### Scenario: `get_group_id(dim)` with non-uniform dim does not prove `%v0` uniform
- **GIVEN** a function has `%v0` that is not proven uniform before a call
- **AND GIVEN** the function calls `_Z12get_group_idj` or `_Z15get_global_sizej`
- **WHEN** CFG verification computes vector-uniform facts after the call
- **THEN** `%v0` is not proven uniform solely because the returned component is work-group-uniform for a fixed dimension

#### Scenario: Pure math builtin preserves uniformity from uniform inputs
- **GIVEN** a function calls an inlined pure math builtin such as `_Z4sqrtf`, `_Z4fmaxff`, or `_Z5mad24iii`
- **WHEN** all vector input registers required by the builtin summary are proven uniform
- **THEN** the builtin result register is proven uniform
- **AND WHEN** any required input register is not proven uniform
- **THEN** the builtin result register is not proven uniform

#### Scenario: Emitter builtin lookup and verifier summaries stay in sync
- **WHEN** a helper name is added to or removed from the PTX emitter builtin inline lookup
- **THEN** the verifier-visible summary table is updated in the same change
- **AND THEN** automated checks fail if an inlined builtin has no verifier-visible summary
- **AND THEN** a resolved emitter-accepted builtin without a verifier-visible summary is reported as metadata drift rather than silently using an ordinary-call conservative path

### Requirement: Barrier legality SHALL use call-aware vector-uniform facts

The system SHALL evaluate `barrier` legality using vector-uniform facts after applying builtin helper summaries and explicit conservative handling for direct calls that have no verifier-visible summary.

If a `barrier` lies in the region of a `vbranch` that cannot be proven uniform after call-aware analysis, translation MUST fail before PTX emission.

#### Scenario: Divergent builtin-derived branch rejects barriers before PTX emission
- **GIVEN** a function calls `_Z12get_local_idj`
- **AND GIVEN** a later `vbranch` uses the returned local ID in a branch condition
- **AND GIVEN** one or more `barrier` instructions lie inside the branch region before reconvergence
- **WHEN** `sbt_decode cfgverify` or `sbt_ptx --require-known` analyzes the function
- **THEN** verification fails with a barrier convergence diagnostic
- **AND THEN** `sbt_ptx` does not emit PTX that can hang in CUDA `bar.sync`

#### Scenario: Uniform builtin-derived branch may pass barrier verification
- **GIVEN** a function calls a builtin whose summary proves a work-group-uniform result from the current input facts
- **AND GIVEN** a later `vbranch` uses only proven-uniform operands
- **WHEN** a `barrier` appears outside any unproven divergent branch region
- **THEN** the barrier is not rejected merely because a builtin call occurred earlier

### Requirement: Direct calls without verifier-visible summaries SHALL have explicit conservative treatment

For direct calls that do not have a verifier-visible summary, this change SHALL treat the call as a conservative verifier boundary unless a future change adds non-builtin summaries or interprocedural analysis.

The verifier MUST NOT silently preserve vector-uniform facts across an unanalyzed direct call. The first implementation SHALL clear all vector-uniform facts across such calls.

#### Scenario: Resolved helper call without summary clears vector-uniform facts
- **GIVEN** a function contains a direct `jal` to a resolved non-builtin helper
- **AND GIVEN** no verifier summary or interprocedural proof exists for that helper
- **WHEN** vector-uniform analysis crosses the call
- **THEN** the verifier clears all vector-uniform facts
- **AND THEN** any resulting barrier rejection is reported as a normal fail-fast verifier diagnostic

#### Scenario: Missing symbol map does not silently assume builtin behavior
- **GIVEN** a direct call is present in a CFG
- **AND GIVEN** the verifier was invoked without call-target symbol information
- **WHEN** vector-uniform analysis crosses the call
- **THEN** the call is handled as an explicit conservative boundary that clears all vector-uniform facts
- **AND THEN** builtin-specific uniformity is applied only when the call target can be resolved to a summarized builtin
