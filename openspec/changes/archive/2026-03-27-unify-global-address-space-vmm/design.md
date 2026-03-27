## Context

> Status: `target behavior`

This design describes the target implementation for change `unify-global-address-space-vmm`.

Canonical references this design depends on:

- `openspec/changes/unify-global-address-space-vmm/specs/global-address-space/spec.md`
- `openspec/changes/unify-global-address-space-vmm/specs/ptx-call-prototype/spec.md`
- `openspec/specs/ptx-call-prototype/spec.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `README.md`

Current implemented behavior still reflects the historical split:

- PTX ordinary address mapping distinguishes `shared` vs `ELF global` vs `heap global`
- PTX kernel entry ABI carries both `elf_base` and `heap_base`
- helper runtime environment carries both global backing pointers
- PTX driver allocates separate eager CUDA buffers for ELF image bytes and heap allocations

The target behavior is to collapse all ordinary non-shared memory into one logical Ventus `Global` region while implementing that region with CUDA sparse virtual memory mappings rather than one monolithic eager allocation.

## Goals / Non-Goals

**Goals:**

- remove runtime-visible `ELF` vs `Heap` distinction from generated PTX
- reduce ordinary load/store lowering to `Shared` vs `Global` selection only
- replace `elf_base + heap_base` ABI fields with one `global_base`
- back the logical `Global` region with CUDA VMM sparse mappings
- preserve fail-fast behavior for invalid addresses and unsupported CUDA environments
- keep documentation and OpenSpec contracts consistent with the new current model

**Non-Goals:**

- preserve external ABI/API compatibility with the old dual-backing interface
- redesign replicated scalar-state, divergence protocol, or direct-call value-ABI layering beyond the backing-pointer shape
- introduce a legacy fallback path when CUDA VMM is unavailable
- guarantee a new public meaning for the historical `0x9000_0000` split beyond any internal allocation convention still used during migration
- optimize PTX instruction count beyond the direct effects of removing `ELF/Heap` runtime selection

## Decisions

### 1. The top-level memory contract becomes `Shared` + `Global`

Translated PTX will treat ordinary runtime-visible memory as exactly two regions:

- `Shared`
- `Global`

`ELF image bytes`, `OpenCL buffer allocations`, `kernel metadata`, `PDS pools`, and internal runtime bookkeeping that is intentionally visible through the Ventus numeric address model all belong to `Global`.

Ordinary PTX lowering will therefore use:

- `.shared` for addresses in the shared window
- `.global` for every other valid translated address

The current `ELF` vs `Heap` distinction is demoted to optional driver-side provenance only.

**Alternatives considered:**
- Keep `ELF` and `Heap` as separate runtime-visible regions: rejected because PTX does not benefit from that distinction and current lowering pays repeated runtime cost for it.
- Collapse to one undifferentiated region including shared: rejected because PTX address-space correctness still requires an explicit `.shared` vs `.global` split.

### 2. PTX ABI uses one `global_base`

The kernel entry ABI will replace:

- `elf_base`
- `heap_base`

with:

- `global_base`

The helper runtime environment blob will likewise replace separate ELF/heap backing fields with one `global_base`.

This change is intentionally end-to-end:

- kernel parameter list
- prologue register assignment
- ordinary address mapping helpers
- helper prototype emission
- helper definition signatures
- runtime environment blob marshal/unmarshal
- driver launch parameter assembly

After this change, generated PTX must not carry enough information to reintroduce runtime branching between ELF-backed and heap-backed ordinary accesses.

**Alternatives considered:**
- Keep old ABI fields but alias them to the same underlying buffer: rejected because it preserves needless contract surface and allows runtime split logic to survive in PTX.
- Change only entry ABI and leave helper runtime environment unchanged: rejected because it would leave the module with two incompatible notions of ordinary global backing.

### 3. Driver Global backing is implemented with CUDA VMM sparse reservation

The PTX driver will reserve one CUDA virtual address range for the logical Ventus `Global` region by using:

- `cuMemAddressReserve`
- `cuMemCreate`
- `cuMemMap`
- `cuMemSetAccess`

The driver will not eagerly allocate one physical CUDA buffer for the whole logical region. Instead:

- ELF PT_LOAD segments are mapped at offsets derived from their Ventus numeric addresses
- runtime allocations receive separately created/mapped handles
- internal buffers such as PDS bitmap storage also use mapped global subranges

This is the correct fit for a sparse logical address space because it decouples:

- logical Ventus numeric address placement
- physical GPU memory commitment

The driver must query CUDA capability support up front and fail immediately when `CU_DEVICE_ATTRIBUTE_VIRTUAL_ADDRESS_MANAGEMENT_SUPPORTED` is absent.

**Alternatives considered:**
- Keep two eager `cuMemAlloc` buffers and merely rename them as one Global region: rejected because it does not solve the sparse-window issue and keeps historical structure in the implementation.
- Use one large eager `cuMemAlloc` for all of Global: rejected because it overcommits physical memory for unused ranges and makes sparse address management less explicit.

### 4. VMM mappings are managed per allocation-aligned chunk, not as one giant physical handle

CUDA VMM imposes mapping granularity and lifecycle constraints:

- physical allocation size must respect `cuMemGetAllocationGranularity`
- access must be enabled with `cuMemSetAccess`
- unmapping works at mapping boundaries rather than arbitrary byte ranges

Therefore the driver will manage `Global` backing as a set of aligned mapped chunks / handles rather than as one single physical allocation handle for the entire region.

Practical consequences:

- ELF PT_LOAD upload may map one or more aligned chunks covering the segment ranges
- each runtime allocation should own one or more VMM-backed mapped ranges
- freeing runtime allocations can unmap/release only their owned handles
- internal bookkeeping must track numeric address range, reserved CUDA VA, physical handle, mapped size, and access state

This chunked design keeps the free path explicit and avoids depending on unsupported subrange manipulation of a giant unified handle.

**Alternatives considered:**
- Use one physical handle for the whole Global window: rejected because it defeats sparse physical commitment and makes later unmap/free behavior awkward.
- Hide VMM granularity with ad hoc over-allocation and silent reuse: rejected because it obscures correctness and complicates fail-fast debugging.

### 5. Numeric address validity stays explicit, but `0x9000_0000` stops being a PTX semantic boundary

The translated PTX still needs a numeric base for Global address calculation, but `0x9000_0000` will no longer be a runtime semantic branch point inside PTX.

The new ordinary access logic is:

- if address is in the shared window: `.shared`
- else if address is in the supported non-shared translated range: `.global` via `global_base + (addr - global_base_vaddr)`
- else: explicit failure / trap according to the surrounding fail-fast contract

Internal driver allocation policy may still choose to place runtime allocations above a certain numeric threshold during migration, but that threshold is no longer part of generated PTX semantics.

This also applies to PDS-related ordinary accesses: they remain part of `Global`, not a separate ordinary address-space class.

**Alternatives considered:**
- Preserve `0x9000_0000` as a PTX-visible runtime split for compatibility: rejected because the user explicitly wants `ELF/Heap` to belong to the same PTX region.
- Remove all explicit numeric validity checks immediately: rejected because invalid translated addresses must still fail fast rather than silently land in arbitrary mapped pages.

## Risks / Trade-offs

- CUDA VMM support becomes a hard dependency of the PTX backend path touched by this change. Unsupported devices will now fail earlier and more explicitly.
- Driver memory management becomes more complex. The implementation must track reservation ranges, map handles, access descriptors, granularity-aligned sizes, and teardown order correctly.
- Existing helper tests and ABI assertions will need coordinated updates because changing `runtime_env_blob` layout affects prototypes, definitions, and call-site marshaling simultaneously.
- Documentation must be updated in one sweep. Leaving `README.md`, `doc/IMPLEMENTATION_CODEMAP.md`, or `doc/ADDRESS_SPACE_SPECIALIZATION.md` half-migrated would create conflicting current guidance.
- Some current diagnostics and comments still name `ELF` vs `Heap`; these need deliberate cleanup so the new `Shared + Global` contract is not undermined by stale terminology.
