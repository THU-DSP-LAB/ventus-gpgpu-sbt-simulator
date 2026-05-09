# sbt-rodinia-bringup

> Status: `current`
>
> Canonical current bring-up / fail-fast contract for Rodinia Ventus ELF translation.

## Purpose
Define the prototype requirements for translating Rodinia Ventus ELF kernels through the SBT pipeline (`decode -> CFG verify -> PTX emit`) with explicit fail-fast behavior.

## Requirements

### Requirement: Accept Rodinia Ventus ELF and extract `.text` + symbols
The system SHALL accept the Ventus toolchain produced RISC-V `ELF32` inputs used by `ventus-env/rodinia/opencl/*/*.riscv`, and SHALL extract `.text` and function symbols from `.symtab` without applying relocations.

#### Scenario: Enumerate kernel entry symbols
- **WHEN** the user provides `ventus-env/rodinia/opencl/bfs/object0.riscv` as input
- **THEN** the system identifies function symbols including `BFS_1` and `BFS_2`
- **AND THEN** the system can locate each symbol start address inside `.text`

### Requirement: Decode Ventus instructions with `regext` prefix bundling
The system SHALL decode 32-bit Ventus instructions from `.text` and SHALL implement `regext` as a prefix that only affects the next instruction (bundling into a single logical decoded instruction).

#### Scenario: `regext` affects exactly one subsequent instruction
- **WHEN** decoding a Rodinia kernel that contains `regext`
- **THEN** the decoded stream contains a single logical instruction for the `regext + next` pair
- **AND THEN** the `regext` state is cleared immediately after decoding the next instruction

### Requirement: Build CFG and validate `setrpc/vbranch/join` structural constraints
The system SHALL reconstruct a basic-block CFG for each translated function and SHALL validate that every `vbranch` can be structurally lowered using the `setrpc`-derived join PC (post-dominator, single-entry, no-side-exit).

#### Scenario: Pass verification on Rodinia BFS kernels
- **WHEN** analyzing `ventus-env/rodinia/opencl/bfs/object0.riscv` for kernel functions
- **THEN** every `vbranch` in the analyzed kernels is either (a) verified as structurally reducible, or (b) the input is rejected with a clear diagnostic

### Requirement: Define supported vs unsupported features and fail fast
The system SHALL explicitly define the supported instruction subset and control-flow forms for the prototype stage, and SHALL reject unsupported inputs with clear error messages.

#### Scenario: Reject unsupported instructions or control flow
- **WHEN** the input contains `regexti` or a non-return `jalr` inside a translated function
- **THEN** the system stops translation and reports an "unsupported" diagnostic that names the instruction/form and its PC

### Requirement: Enforce `barrier` legality at converged points
The system SHALL treat Ventus `barrier` as CUDA `__syncthreads()` / PTX `bar.sync` and SHALL reject inputs where a `barrier` may execute on a diverged path.

#### Scenario: Barrier must be provably converged
- **WHEN** a function contains `barrier`
- **THEN** the system verifies the control-flow constraints that guarantee convergence at that point
- **AND THEN** translation fails with a diagnostic if convergence cannot be proven

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

### Requirement: Direct calls without verifier-visible summaries SHALL have explicit conservative treatment
For direct calls that do not have a verifier-visible summary, the system SHALL treat the call as an explicit ABI-aware verifier boundary unless a future change adds non-builtin summaries or stronger interprocedural analysis.

The verifier MUST NOT silently preserve caller-saved vector-uniform facts across an unanalyzed direct call. The current implementation clears caller-saved `%v0..%v31` vector-uniform facts across such calls and preserves callee-saved `%v32..%v255` facts according to the Ventus call ABI.

#### Scenario: Resolved helper call without summary clears caller-saved vector-uniform facts
- **GIVEN** a function contains a direct `jal` to a resolved non-builtin helper
- **AND GIVEN** no verifier summary or interprocedural proof exists for that helper
- **WHEN** vector-uniform analysis crosses the call
- **THEN** the verifier clears caller-saved `%v0..%v31` vector-uniform facts
- **AND THEN** the verifier preserves callee-saved `%v32..%v255` vector-uniform facts

#### Scenario: Missing symbol map does not silently assume builtin behavior
- **GIVEN** a direct call is present in a CFG
- **AND GIVEN** the verifier was invoked without call-target symbol information
- **WHEN** vector-uniform analysis crosses the call
- **THEN** the call is handled as an explicit ABI-aware boundary that clears caller-saved `%v0..%v31` vector-uniform facts

#### Scenario: Reachable callee receives call-site entry facts
- **GIVEN** `sbt_ptx` translates a kernel plus reachable direct-call callees
- **AND GIVEN** every call site to a callee proves a callee entry vector register uniform and proves the call context converged
- **WHEN** the callee contains a `barrier` guarded by a branch over that entry-uniform value
- **THEN** closure verification accepts that barrier
- **AND WHEN** any call site enters the callee from a non-converged context
- **THEN** closure verification rejects reachable barriers in that callee

### Requirement: Translate kernel entry functions by name and skip `_start` in prototype mode
The system SHALL translate a selected kernel entry function identified by name (e.g. from runtime `kernel_name`) using ELF `.symtab`, and SHALL NOT require translating the ELF entrypoint `_start` in prototype mode.

#### Scenario: `_start` contains an indirect jump but kernel translation still proceeds
- **WHEN** the user requests translation of `BFS_1` from `ventus-env/rodinia/opencl/bfs/object0.riscv`
- **THEN** translation starts from the `BFS_1` function symbol and its reachable callees
- **AND THEN** the existence of `jalr t1` in `_start` does not cause translation to fail
