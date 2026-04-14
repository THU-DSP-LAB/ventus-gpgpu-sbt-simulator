# ptx-temp-register-allocation

> Status: `current`
>
> Canonical current contract for PTX emitter scratch-register allocation and `%tmp*` virtual temporary ownership.

## Purpose
Define how the PTX emitter separates fixed machine/runtime/control slots from lowering scratch so helper-local temporaries no longer depend on a shared fixed numeric scratch pool.

## Requirements

### Requirement: PTX emitter SHALL allocate uniquely named virtual temporary registers for scratch use
The PTX emitter MUST provide an explicit allocation mechanism for temporary PTX registers used only as lowering scratch state.

For each PTX scratch kind used by the emitter, the allocator MUST be able to produce uniquely named temporary registers without relying on a shared fixed numeric scratch pool.

At minimum, this applies to temporary registers used as:
- scalar integer scratch
- 64-bit address scratch
- predicate scratch
- floating-point scratch
- half / packed-element scratch

The emitted temporary names MAY use any stable `%tmp*` naming convention, as long as each allocated temporary name is unique within the containing PTX function scope.

#### Scenario: Two helpers no longer implicitly share `%r14`
- **GIVEN** two emitter helpers both require a 32-bit temporary register
- **WHEN** they are lowered in the same PTX function
- **THEN** each helper receives its own uniquely named temporary register
- **AND THEN** correctness does not depend on manually preserving a shared `%r14/%r15/...` scratch convention

### Requirement: Fixed machine/runtime slots SHALL remain distinct from virtual temporary registers
The new virtual temporary-register allocation path MUST NOT redefine or absorb PTX registers whose meaning is part of the current machine/runtime lowering contract.

This exclusion applies at least to:
- `%x<...>` and `%v<...>` logical Ventus register files
- persistent leader metadata such as `leader_lane`
- fixed machine-context slots
- fixed runtime-environment slots
- any other PTX register whose meaning is explicitly documented as part of the current emitter contract

#### Scenario: Temp allocation does not rename machine context slots
- **GIVEN** the emitter lowers code that reads `knl_vaddr`, `warp_id_in_block`, and `global_base`
- **WHEN** temporary registers are allocated for surrounding scratch work
- **THEN** the documented fixed machine/runtime slots remain separate from `%tmp*` scratch registers
- **AND THEN** the change does not require renumbering the current fixed-slot contract

### Requirement: First-stage migration SHALL keep function-scope declaration semantics
The first implementation of virtual temporary registers MAY keep PTX register declarations at function scope rather than introducing block-local `{}` scopes throughout the emitter.

However, each temporary register MUST still be declared before use and within the same PTX function scope that uses it.

The first-stage migration MUST NOT depend on broad control-flow restructuring solely to enable `%tmp*` adoption.

#### Scenario: `%tmp*` migration does not require block-local scope insertion
- **GIVEN** an existing lowering helper currently uses fixed scratch registers inside a PTX function
- **WHEN** that helper is migrated to uniquely named `%tmp*` temporaries in the first stage
- **THEN** the helper may continue to rely on function-scope `.reg` declarations
- **AND THEN** the migration does not require wrapping the helper in additional PTX lexical blocks to remain valid

### Requirement: Temporary-register migration SHALL cover emitter scratch ownership, not architectural ABI redesign
The purpose of the temporary-register migration is to make scratch-register ownership explicit and collision-free.

The change MUST NOT treat virtual temporary-register allocation as justification to redesign:
- direct-call value ABI layout
- current fixed machine/runtime slot numbering
- logical Ventus `%x/%v` state representation

#### Scenario: Temp-register migration preserves existing ABI contracts
- **GIVEN** a PTX helper call still uses the current mutable-state, machine-context, and runtime-env blobs
- **WHEN** scratch registers inside the caller or callee are migrated to `%tmp*`
- **THEN** the helper ABI remains semantically unchanged
- **AND THEN** the change is classified as emitter scratch-management refactoring rather than ABI redesign
