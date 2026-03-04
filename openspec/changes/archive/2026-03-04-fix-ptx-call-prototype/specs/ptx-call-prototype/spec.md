## ADDED Requirements

### Requirement: Declare PTX call prototypes for all emitted helper functions
When emitting a multi-function PTX module, the emitter SHALL declare a call prototype for every emitted non-entry helper `.func` before any function body that may call it.

#### Scenario: Forward call to helper function
- **GIVEN** a kernel function calls helper `A`
- **AND GIVEN** helper `A` appears after the caller in the emitted function-definition order
- **WHEN** the PTX module is consumed by CUDA JIT (`cuModuleLoadDataEx`/ptxas)
- **THEN** the module is accepted without `requires call prototype` or `Unknown symbol` errors for `A`

### Requirement: Prototype signature must match emitted helper definition
Each declared helper prototype MUST use the exact same parameter list and types as the corresponding emitted helper `.func` definition.

#### Scenario: Call argument list remains ABI-compatible
- **GIVEN** helper calls pass through `elf_base`, `heap_base`, `wctx_ptr`, `lds_ptr`, `knl_vaddr`, `pds_base_vaddr`, `pds_size_per_thread`, `warp_id`, `warps_per_block`
- **AND GIVEN** vector-context mode additionally passes `vctx_base`
- **WHEN** PTX is emitted and assembled
- **THEN** each call target prototype and definition share identical signature shape

## MODIFIED Requirements

### Requirement: Multi-function PTX emission no longer depends on definition order for call resolution
The multi-function PTX emission flow SHALL remain functionally equivalent for call lowering, while call target resolution is made order-independent through explicit prototypes.

#### Scenario: Existing call lowering still works after prototype addition
- **GIVEN** a module with direct calls to emitted helper functions
- **WHEN** the module is emitted with prototype declarations
- **THEN** call instructions still target the same helper symbol names and runtime behavior is unchanged
