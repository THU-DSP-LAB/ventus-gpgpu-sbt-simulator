## Why

Current PTX lowering still treats Ventus non-shared numeric addresses as two runtime-selected global backings:

- `ELF` backing for `[0x8000_0000, 0x9000_0000)`
- `Heap` backing for `[0x9000_0000, ...)`

This split is no longer a good fit for the actual execution model:

- PTX/CUDA only needs a top-level distinction between `.shared` and `.global`
- current load/store lowering pays repeated runtime cost to distinguish `ELF` vs `Heap`
- kernel entry ABI and helper runtime environment still carry both `elf_base` and `heap_base`
- driver memory management allocates and routes two separate CUDA global buffers even though both represent the same logical Ventus global space

For this project, `ELF` image bytes, argument buffers, metadata buffers, ordinary OpenCL buffers, and PDS backing should all belong to one logical Ventus `Global` region. The current `ELF/Heap` split is historical implementation structure, not the target contract we want to preserve.

At the same time, allocating one eagerly materialized CUDA buffer for the entire logical global window is wasteful and makes sparse address ranges harder to manage. CUDA Driver VMM provides a better fit: reserve one virtual global range, then map only the physical chunks that are actually needed.

## What Changes

This change will redefine the current address-space contract and implementation around two top-level runtime-visible regions:

- `Shared`
- `Global`

The change will:

- remove PTX runtime selection between `elf_base` and `heap_base`
- replace the PTX kernel entry ABI with a single `global_base` backing parameter
- replace helper `runtime_env_blob` fields `elf_base + heap_base` with one `global_base`
- change ordinary address mapping helpers so they distinguish only `shared` vs `global`
- keep explicit fail-fast behavior for out-of-range numeric addresses and unsupported VMM environments
- implement driver global backing with CUDA Virtual Memory Management (`cuMemAddressReserve`, `cuMemCreate`, `cuMemMap`, `cuMemSetAccess`) instead of two separately allocated CUDA buffers

This change does **not** require preserving ABI/API compatibility with existing external users.

This change also does **not** require keeping a runtime-visible `ELF`/`Heap` region distinction in generated PTX. If desired, internal provenance such as "mapped from ELF image" vs "allocated by runtime" may remain as driver-side bookkeeping only.

### Documentation Impact

The following long-lived docs will need updates to match the new current contract:

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `doc/ADDRESS_SPACE_SPECIALIZATION.md`
- `openspec/README.md`
- `openspec/specs/ptx-call-prototype/spec.md`
- any current spec updated or added by this change

This change supersedes the remaining active documentation framing that still describes ordinary non-shared accesses as `ELF backing` vs `heap backing` at the current-contract level. Historical notes under `doc/archive/` may remain for background, but `doc/` root must keep only one active current description of the address-space model.

## Capabilities

### New Capabilities

- Define a single Ventus `Global` runtime-visible region backed by CUDA VMM sparse mappings
- Launch translated PTX kernels with one `global_base` parameter instead of separate `elf_base` and `heap_base`
- Fail fast when CUDA VMM is unavailable instead of silently falling back to the legacy dual-backing model

### Modified Capabilities

- Ordinary scalar/vector memory lowering will classify addresses as `Shared`, `Global`, or invalid, rather than `Shared`, `ELF`, or `Heap`
- Direct-call runtime environment transport will carry one global backing pointer instead of two global backing pointers
- PTX driver memory management will upload ELF PT_LOAD segments and runtime allocations into one logical global space

## Impact

This change simplifies the PTX lowering contract, reduces repeated runtime address-selection logic, and aligns the implementation with the actual target memory spaces exposed by PTX.

It also raises the implementation bar in the driver:

- CUDA VMM support becomes a hard requirement for the PTX backend path covered by this change
- mapping granularity and sparse-map lifecycle must be managed explicitly
- driver allocation bookkeeping must distinguish logical Ventus numeric addresses from CUDA-reserved virtual address reservations cleanly

Once this change lands, the current contract should describe Ventus runtime memory as:

- `Shared`: numeric shared/LDS window
- `Global`: all non-shared translated kernel-visible memory, including ELF image contents and runtime-allocated buffers
