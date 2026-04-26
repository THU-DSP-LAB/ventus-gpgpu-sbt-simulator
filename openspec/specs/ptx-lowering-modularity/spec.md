# ptx-lowering-modularity Specification

## Purpose
定义当前 PTX emitter 的模块化结构合同，明确 internal shared interface、implementation ownership units、domain-specific lowering units、instruction dispatcher precedence 与结构化验证覆盖面的 current 要求。
## Requirements
### Requirement: PTX emitter SHALL separate shared emission core from domain-specific lowering units
The PTX emitter implementation MUST maintain a clear boundary between:

- shared emission core responsibilities
- domain-specific lowering responsibilities

The shared emission core is the authority for function/module assembly and shared lowering contract. It MUST expose the shared contract through `sbt/ptx_emit_internal.hpp` / `EmitCtx` and MUST own at least:

- PTX module/function header and body assembly
- fixed register and blob ABI contract
- temporary-register and label allocation
- shared address-mapping helpers
- entry/helper prologue and epilogue logic
- CFG block traversal and fallthrough emission

Domain-specific lowering units and internal ownership units MUST consume this shared core contract rather than each redefining their own copies of these responsibilities.

#### Scenario: Scalar lowering does not redefine module assembly
- **GIVEN** the emitter lowers a current supported scalar instruction
- **WHEN** the scalar lowering path emits PTX
- **THEN** module/function body assembly, temporary allocation, and blob ABI semantics still come from the shared emission core
- **AND THEN** the scalar lowering unit does not carry its own duplicate module-assembly or ABI contract

### Requirement: PTX emitter internal header SHALL expose shared interface rather than bulk implementation
The PTX emitter internal header MUST serve as the shared internal interface for the emitter host and lowering units, not as a bulk implementation container.

It MAY contain:

- shared type declarations and small value-like structs
- `EmitCtx` public internal interface declarations
- constants that define current ABI/register/temp contracts
- small inline helpers where keeping them inline materially improves clarity without hiding domain-specific lowering
- declarations for domain-level and helper-level implementation functions

It MUST NOT own large bodies of implementation for:

- entry/helper prologue and PDS pool acquire/release
- shared/global numeric address mapping and typed load/store sequences
- mutable/machine/runtime blob marshal and direct-call emission
- builtin inline lowering bodies
- scalar FP lowering bodies
- MMA tuple materialization, shuffle gather/merge, native/composite MMA emission
- final function-body assembly beyond interface-level declarations

The implementation MAY keep tightly coupled micro-primitives inline, but large semantic routines MUST live in `.cpp` files whose names and build ownership match their responsibilities.

#### Scenario: Domain lowering unit includes only required internal interface
- **GIVEN** a domain lowering file such as scalar, vector, custom, or MMA lowering includes the emitter internal header
- **WHEN** a maintainer inspects that header
- **THEN** the header exposes the shared host contract and helper declarations needed by the domain
- **AND THEN** the maintainer does not have to read unrelated builtin, PDS, scalar-FP, or MMA materialization implementations inside the header to understand the interface boundary

#### Scenario: Internal header does not become the new monolith
- **GIVEN** PTX emitter implementation is modularized across multiple translation units
- **WHEN** a new large lowering or runtime helper is added
- **THEN** it is placed in an ownership-appropriate `.cpp` unit
- **AND THEN** it is not added wholesale to `sbt/ptx_emit_internal.hpp` merely because multiple lowering units need to call it

### Requirement: PTX emitter implementation ownership SHALL be explicit for internal helper domains
The modularized PTX emitter MUST maintain an explicit implementation ownership map for internal helper domains that are shared by, or adjacent to, instruction lowering.

This ownership map defines where method bodies and private helper routines live. It MUST NOT split the shared semantic authority for fixed registers, virtual temps, blob layout, address mapping, leader selection, or dispatcher preconditions into multiple independent contracts. Ownership units remain implementation units behind the shared emitter host/interface.

At minimum, ownership MUST remain coherent for:

- core/function assembly: PTX function headers, register declarations, dispatcher, CFG block traversal, fallthrough emission
- runtime/PDS: entry/helper prologue, PDS acquire/release, prologue-time machine/runtime context restore, CSR-derived runtime helpers
- memory/address mapping: numeric address classification, shared/global pointer materialization, typed load/store helper sequences, leader-only scalar store wrappers
- call ABI: helper signatures, call parameter layout, mutable-state blob marshal for helper entry/exit and direct call, outgoing machine/runtime blob marshal, direct `.func` call emission
- builtin lowering: builtin allowlist, builtin inline dispatch, builtin inline emission bodies
- scalar FP lowering: rounding normalization, `fclass`, scalar FP instruction lowering
- MMA lowering: metadata validation, tuple materialization, shuffle gather/merge, native/composite MMA PTX emission

Each ownership unit MUST consume shared core primitives rather than copying fixed register, temp allocation, blob ABI, address mapping, or dispatch-precondition contracts.

#### Scenario: MMA lowering implementation lives with MMA ownership
- **GIVEN** a current supported MMA instruction reaches PTX emission
- **WHEN** the emitter materializes PTX tuples and emits native or composite MMA operations
- **THEN** the implementation resides in the MMA lowering ownership unit
- **AND THEN** the MMA lowering unit continues to consume `sbt/ptx_mma.*` planner/ABI helpers and shared `EmitCtx` primitives rather than duplicating those contracts

#### Scenario: Builtin allowlist and inline dispatch stay coherent
- **GIVEN** a direct call target is an emitter-inlined builtin
- **WHEN** `is_inlined_builtin_call_name()` accepts that symbol
- **THEN** the builtin inline dispatch has a matching implementation path for that accepted symbol
- **AND THEN** the builtin path remains owned by the direct-call/builtin ownership unit rather than being split across unrelated scalar/vector/math files

#### Scenario: Ownership units do not become independent contract authorities
- **GIVEN** runtime/PDS, call ABI, memory, and MMA helpers are implemented in separate `.cpp` files
- **WHEN** those helpers need fixed registers, temp allocation, blob offsets, address windows, or dispatcher preconditions
- **THEN** they consume the shared emitter host/interface contract
- **AND THEN** they do not define a second local version of those contracts inside their ownership file

#### Scenario: Shared helpers are not copied into ownership units
- **GIVEN** scalar and vector lowering both need numeric address mapping
- **WHEN** their lowering implementations emit loads or stores
- **THEN** both use the shared memory/address-mapping helper contract
- **AND THEN** neither lowering unit carries an independent local copy of the fixed address-window rules

### Requirement: Lowering-unit boundaries SHALL follow semantic authority rather than file-size goals
When splitting PTX lowering into multiple implementation units, the boundary MUST be chosen by semantic lowering authority and shared contract, not by line-count reduction alone.

At minimum, the modularized design MUST preserve coherent ownership for these domains:

- control-flow and structured-control lowering, including direct-call and builtin-call resolution
- scalar memory / scalar integer / scalar floating-point lowering
- vector memory / vector register / vector integer / vector floating-point / compare / convert / mask lowering
- custom non-MMA lowering
- MMA PTX materialization

The implementation MUST NOT split tightly coupled lowering logic into separate units solely to reduce file size if doing so would duplicate shared helpers, duplicate contract checks, or obscure the true ownership boundary.

#### Scenario: Builtin call lowering remains in the control/call domain
- **GIVEN** an inlined builtin call eventually produces scalar, vector, or math-like PTX instructions
- **WHEN** the modularized emitter assigns ownership of that lowering path
- **THEN** the ownership remains attached to the direct-call/control resolution domain that decides the callee behavior
- **AND THEN** the implementation does not move it to another unit solely because the resulting PTX looks like scalar/vector math

### Requirement: Instruction dispatch SHALL be a thin domain dispatcher with explicit precedence
The instruction-level PTX emission entrypoint MUST become a thin dispatcher that selects lowering by explicit semantic domain precedence rather than by a single monolithic inlined branch chain.

The precedence between domain handlers MUST remain explicit and reviewable as part of the emitter contract.

The dispatcher MAY short-circuit through domain-specific `try_emit_*` style helpers, but correctness MUST NOT depend on an implicit ordering hidden inside one large mixed-domain function body.

The dispatcher/core boundary MUST remain the central authority for shared preconditions that apply before domain-specific lowering begins. These shared preconditions include comment emission and any repository-managed fail-fast validation that is intended to run before domain dispatch on the current supported path, such as scalar-execution classification metadata gates when required.

Domain-specific lowering units MUST NOT silently re-scatter these shared preconditions into per-domain copies if doing so would obscure the single reviewable entrypoint for current supported-path dispatch.

#### Scenario: Domain precedence is visible without reading every lowering branch
- **GIVEN** a maintainer needs to confirm whether a current supported instruction is handled by control, scalar, vector, custom, or MMA lowering
- **WHEN** they inspect the instruction-dispatch entrypoint
- **THEN** the domain ordering is visible directly in the dispatcher structure
- **AND THEN** they do not need to scan a monolithic mixed-domain branch chain to infer the precedence contract

### Requirement: Internal split SHALL preserve current PTX behavior
Splitting the PTX emitter internal core MUST be behavior-preserving for current supported PTX emission.

The split MUST NOT change:

- `sbt/ptx_emit.hpp` public API
- `emit_module()` / `emit_kernel()` observable output contract except for non-semantic formatting that remains accepted by current tests
- descriptor/payload authority for ordinary/custom/MMA lowering
- dispatcher precedence: `control -> scalar -> vector -> mma -> custom`
- fixed register and `%tmp*` virtual temp ownership
- replicated scalar-state behavior
- direct-call value ABI and helper prototype/definition consistency
- shared/global numeric address mapping contract
- PDS acquire/release semantics
- current MMA supported family behavior
- allowed `DecodedInst.name` uses

All structural validation and regression entrypoints that protect these contracts MUST continue to cover the full post-split emitter file set.

#### Scenario: Public emitter API remains stable
- **GIVEN** a caller uses `sbt::ptx::emit_module()` or `sbt::ptx::emit_kernel()`
- **WHEN** the internal split is implemented
- **THEN** the caller does not need to change source code
- **AND THEN** the same current supported inputs continue to emit PTX through the same public API

#### Scenario: Dispatcher precedence remains explicit after the internal split
- **GIVEN** an instruction could be associated with custom metadata and MMA metadata ownership
- **WHEN** instruction emission dispatches to domain handlers
- **THEN** the dispatcher order remains visibly `control -> scalar -> vector -> mma -> custom`
- **AND THEN** generic custom lowering does not consume current MMA paths before MMA lowering can validate them

### Requirement: Structural validation SHALL cover all emitter lowering translation units
The project's structural validation for PTX lowering authority and name-usage restrictions MUST remain effective after emitter modularization.

If current validation or static checks previously assumed a single `sbt/ptx_emit.cpp`, or only the first domain-split file set, they MUST be updated so that:

- supported-path authority checks still cover every lowering and internal ownership translation unit
- allowed external/comment/diagnostic uses of mnemonic names remain explicitly scoped
- external mnemonic contract regressions still cover comments / diagnostics / external builtin symbol behavior after the split
- builtin public allowlist, builtin lookup, and inline dispatch remain synchronized
- compile-first and relevant emitter regressions continue to validate the modularized implementation as one coherent emitter

Emitter modularization MUST NOT reduce validation strength by leaving newly introduced lowering units outside the existing structural checks.

#### Scenario: Name-allowlist validation still covers modularized lowering files
- **GIVEN** the emitter has been split into multiple lowering implementation files
- **WHEN** structural validation checks mnemonic-name usage on the PTX lowering path
- **THEN** the check covers the full modularized emitter file set
- **AND THEN** it does not silently stop enforcing the authority boundary for files moved out of the original monolithic source

#### Scenario: Builtin lookup validation covers the call closure boundary
- **GIVEN** the public call-graph closure accepts emitter-inlined builtin symbols
- **WHEN** structural validation runs
- **THEN** every accepted builtin symbol maps through the shared builtin lookup to an inline dispatch path
- **AND THEN** the closure cannot accept a builtin name that control lowering cannot emit
