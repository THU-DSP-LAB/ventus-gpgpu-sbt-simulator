# ptx-call-prototype

> Status: `current`
>
> Canonical current contract for multi-function PTX helper prototypes and value-ABI-compatible signatures.

## Purpose
Define module-level PTX helper call prototype requirements so multi-function SBT PTX remains valid under forward-call ordering.

## Requirements

### Requirement: Declare PTX call prototypes for all emitted helper functions
When emitting a multi-function PTX module, the emitter SHALL declare a call prototype for every emitted non-entry helper `.func` before any function body that may call it.

#### Scenario: Forward call to helper function
- **GIVEN** a kernel function calls helper `A`
- **AND GIVEN** helper `A` appears after the caller in the emitted function-definition order
- **WHEN** the PTX module is consumed by CUDA JIT (`cuModuleLoadDataEx`/ptxas)
- **THEN** the module is accepted without `requires call prototype` or `Unknown symbol` errors for `A`

### Requirement: Prototype signature must match emitted helper definition under the new value ABI
Each declared helper prototype MUST use the exact same parameter list and types as the corresponding emitted helper `.func` definition after the direct-call value ABI is adopted.

For the new helper ABI, the prototype/definition signature MUST track the current value-ABI state layering, including a runtime environment shape that carries one logical `global_base` backing pointer rather than separate ELF and heap backing pointers.

#### Scenario: Helper prototype follows the new value ABI shape
- **GIVEN** a translated module emits helper `.func` definitions using the new value ABI layering for mutable call state, read-only machine context, and runtime environment
- **AND GIVEN** the current runtime environment carries one `global_base` backing pointer for ordinary non-shared memory
- **WHEN** the PTX module emits forward-call helper prototypes
- **THEN** each helper prototype and helper definition share the same new ABI signature shape
- **AND THEN** prototype emission does not continue to require separate `elf_base` and `heap_base` runtime-environment fields

### Requirement: Prototype-based forward-call validity SHALL remain after ABI migration
The move from the legacy helper ABI to the new value ABI MUST NOT reintroduce forward-call ordering dependence.

#### Scenario: Forward helper call still assembles after ABI migration
- **GIVEN** helper `A` is called before its definition appears in the emitted PTX text
- **AND GIVEN** helper `A` now uses the new value ABI parameter layout
- **WHEN** the PTX module is assembled or JIT-loaded
- **THEN** the module is accepted without prototype-related call resolution errors
- **AND THEN** the helper call targets the new ABI-compatible prototype rather than a stale legacy one
