# ptx-call-prototype (delta spec)

## MODIFIED Requirements

### Requirement: Prototype signature must match emitted helper definition under the current value ABI
Each declared helper prototype MUST use the exact same parameter list and types as the corresponding emitted helper `.func` definition under the current direct-call value ABI.

For the current helper ABI, the prototype/definition signature MUST track the current value-ABI state layering, including a runtime environment shape that carries one logical global backing pointer rather than separate ELF and heap backing pointers.

#### Scenario: Helper prototype follows the single-Global runtime environment shape
- **GIVEN** a translated module emits helper `.func` definitions using the current value ABI layering for mutable call state, read-only machine context, and runtime environment
- **AND GIVEN** the current runtime environment carries one `global_base` backing pointer for ordinary non-shared memory
- **WHEN** the PTX module emits forward-call helper prototypes
- **THEN** each helper prototype and helper definition share the same current ABI signature shape
- **AND THEN** prototype emission does not continue to require separate `elf_base` and `heap_base` runtime-environment fields
