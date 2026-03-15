# ptx-call-prototype (delta spec)

## MODIFIED Requirements

### Requirement: Prototype signature must match emitted helper definition under the new value ABI
Each declared helper prototype MUST use the exact same parameter list and types as the corresponding emitted helper `.func` definition after this change's direct-call value ABI is adopted.

For the new helper ABI, the prototype/definition signature MUST track the current value-ABI state layering rather than the legacy fixed argument list built around `wctx_ptr`, `lds_ptr`, and optional `vctx_base`.

#### Scenario: Helper prototype follows the new value ABI shape
- **GIVEN** a translated module emits helper `.func` definitions using the new value ABI layering for mutable call state, read-only machine context, and runtime environment
- **WHEN** the PTX module emits forward-call helper prototypes
- **THEN** each helper prototype and helper definition share the same new ABI signature shape
- **AND THEN** prototype emission does not continue to require the legacy `elf_base ... vctx_base` parameter list

### Requirement: Prototype-based forward-call validity SHALL remain after ABI migration
The move from the legacy helper ABI to the new value ABI MUST NOT reintroduce forward-call ordering dependence.

#### Scenario: Forward helper call still assembles after ABI migration
- **GIVEN** helper `A` is called before its definition appears in the emitted PTX text
- **AND GIVEN** helper `A` now uses the new value ABI parameter layout
- **WHEN** the PTX module is assembled or JIT-loaded
- **THEN** the module is accepted without prototype-related call resolution errors
- **AND THEN** the helper call targets the new ABI-compatible prototype rather than a stale legacy one
