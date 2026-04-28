# ptx-lowering-modularity (delta spec)

## ADDED Requirements

### Requirement: PTX emitter internal header SHALL expose shared interface rather than bulk implementation

The PTX emitter internal header MUST serve as the shared internal interface for the emitter host and lowering units, not as a bulk implementation container.

It MAY contain:

- shared type declarations and small value-like structs
- `EmitCtx` public internal interface declarations
- constants that define current ABI/register/temp contracts
- small inline helpers where keeping them inline materially improves clarity without hiding domain-specific lowering
- declarations for domain-level and helper-level implementation functions

It MUST NOT continue to own large bodies of implementation for:

- entry/helper prologue and PDS pool acquire/release
- shared/global numeric address mapping and typed load/store sequences
- mutable/machine/runtime blob marshal and direct-call emission
- builtin inline lowering bodies
- scalar FP lowering bodies
- MMA tuple materialization, shuffle gather/merge, native/composite MMA emission
- final function-body assembly beyond interface-level declarations

The implementation MAY keep tightly coupled micro-primitives inline, but large semantic routines MUST move to `.cpp` files whose names and build ownership match their responsibilities.

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

This ownership map defines where method bodies and private helper routines live. It MUST NOT split the shared semantic authority for
fixed registers, virtual temps, blob layout, address mapping, leader selection, or dispatcher preconditions into multiple independent
contracts. Ownership units remain implementation units behind the shared emitter host/interface.

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

### Requirement: Internal split SHALL preserve current PTX behavior and validation strength

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

#### Scenario: Name allowlist validation covers every new implementation file
- **GIVEN** implementation routines are moved from `ptx_emit_internal.hpp` to new `.cpp` files
- **WHEN** the PTX emit name allowlist check runs
- **THEN** it scans the complete emitter implementation file set
- **AND THEN** it does not allow a new lowering file to reintroduce `DecodedInst.name` as semantic authority outside the explicit allowlist
