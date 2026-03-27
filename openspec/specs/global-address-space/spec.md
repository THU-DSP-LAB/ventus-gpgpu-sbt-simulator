# global-address-space

> Status: `current`
>
> Canonical current contract for PTX backend ordinary address mapping and driver-visible Global backing.

## Purpose
Define the current PTX backend contract for runtime-visible memory regions, ordinary load/store lowering, kernel-entry backing parameters, and driver Global backing behavior.

## Requirements

### Requirement: Runtime-visible non-shared Ventus memory SHALL form one logical Global region
For the PTX backend, translated kernels MUST expose only two top-level runtime-visible memory regions:

- `Shared`
- `Global`

The generated PTX MUST NOT preserve a runtime-visible distinction between former `ELF` backing and former `Heap` backing as separate ordinary memory regions.

#### Scenario: Address in former ELF window uses Global semantics
- **GIVEN** a translated kernel issues an ordinary load/store to a Ventus numeric address in the range previously described as ELF-backed
- **WHEN** the PTX backend lowers that access
- **THEN** the access is treated as a `Global` access rather than a distinct `ELF` access

#### Scenario: Address in former heap window uses the same Global semantics
- **GIVEN** a translated kernel issues an ordinary load/store to a Ventus numeric address in the range previously described as heap-backed
- **WHEN** the PTX backend lowers that access
- **THEN** the access is treated as the same `Global` region rather than a separate heap-only region

### Requirement: Ordinary PTX address mapping SHALL distinguish only Shared vs Global
For ordinary scalar and vector memory operations, the PTX backend MUST classify runtime addresses as:

- `Shared`
- `Global`
- invalid / unsupported

The lowering MUST NOT emit runtime control flow that selects between separate `elf_base` and `heap_base` global pointers.

#### Scenario: Global access uses one base pointer
- **GIVEN** an ordinary load/store whose numeric address is not in the shared window and is otherwise valid for translated execution
- **WHEN** the PTX backend lowers the access
- **THEN** it computes the target address from one `global_base`
- **AND THEN** it does not branch on whether the numeric address originated from an ELF image range or a runtime allocation range

### Requirement: PTX entry ABI and helper runtime environment SHALL carry one Global backing pointer
The translated PTX kernel entry ABI MUST use one global backing pointer for ordinary non-shared memory.

The helper direct-call runtime environment MUST likewise transport one global backing pointer rather than separate ELF and heap backing pointers.

#### Scenario: Kernel launch passes one global backing pointer
- **WHEN** the PTX driver launches a translated kernel
- **THEN** the kernel parameter list contains one `global_base` backing parameter for ordinary non-shared memory
- **AND THEN** the parameter list no longer requires both `elf_base` and `heap_base`

#### Scenario: Helper runtime environment uses one global backing pointer
- **GIVEN** a translated module emits helper `.func` calls using the current layered value ABI
- **WHEN** runtime environment state is marshaled for the helper call
- **THEN** that runtime environment includes one global backing pointer
- **AND THEN** helper lowering does not require separate ELF and heap backing fields

### Requirement: Driver SHALL back the logical Global region with CUDA VMM sparse mappings
The PTX driver MUST implement the logical Ventus `Global` region by reserving CUDA virtual address space and mapping only the physical ranges that are needed for:

- uploaded ELF PT_LOAD segments
- runtime allocations such as OpenCL buffers
- metadata buffers
- PDS pools and internal PDS bookkeeping

The driver MUST NOT silently fall back to the legacy dual-backing allocation model when CUDA Virtual Memory Management is unavailable.

#### Scenario: VMM unsupported environment fails fast
- **GIVEN** the PTX backend is selected on a device or driver configuration that does not support CUDA Virtual Memory Management
- **WHEN** the PTX driver initializes the Global backing
- **THEN** initialization fails with an explicit diagnostic
- **AND THEN** the driver does not silently recreate separate eager `elf_base` and `heap_base` buffers

#### Scenario: Sparse global reservation maps only needed ranges
- **GIVEN** a translated workload uploads a small ELF image and allocates a small number of runtime buffers
- **WHEN** the PTX driver prepares the logical Global region
- **THEN** it reserves the required CUDA virtual address space for the Global region
- **AND THEN** it maps only the physical subranges needed by the uploaded image and active allocations

#### Scenario: Heap-window ELF segment may reuse an already mapped runtime page
- **GIVEN** the PTX driver has already mapped a runtime allocation page in the heap window
- **AND GIVEN** an uploaded ELF `PT_LOAD` segment later lands in the same Global page
- **WHEN** the driver prepares the ELF bytes in Global backing
- **THEN** the driver keeps that page valid for both runtime allocation and ELF image use
- **AND THEN** it does not reject the ELF upload merely because the page was first mapped by runtime allocation

#### Scenario: Small runtime allocation does not consume a whole heap window page of address space
- **GIVEN** the CUDA VMM mapping granularity is larger than a small runtime allocation request
- **WHEN** the PTX driver allocates that runtime buffer in the heap window
- **THEN** it may map the required VMM page backing
- **AND THEN** it advances the logical runtime allocation cursor by the requested aligned size rather than by the full VMM page size

#### Scenario: Global ELF segment outside the configured runtime heap window does not consume heap allocation cursor
- **GIVEN** the PTX driver is configured with a bounded runtime heap window above `0x9000_0000`
- **AND GIVEN** an ELF `PT_LOAD` segment lands in the logical `Global` region but outside that runtime heap window
- **WHEN** the driver uploads the ELF image
- **THEN** the ELF bytes are still backed by the sparse Global mapping
- **AND THEN** the runtime allocation cursor is not advanced beyond the configured heap window because of that segment

#### Scenario: PDS bitmap growth does not require the previous bitmap to remain the top allocation
- **GIVEN** the PTX driver already has an internal PDS bitmap allocation
- **AND GIVEN** later runtime allocations mean that bitmap is no longer the most recent allocation
- **WHEN** a later kernel requires a larger PDS bitmap
- **THEN** the driver may allocate a fresh internal bitmap in Global backing
- **AND THEN** it does not fail merely because the previous bitmap was no longer the last allocation

#### Scenario: Empty Global pages may stay mapped after logical free
- **GIVEN** a runtime allocation releases its last live reference to some Global page
- **WHEN** the PTX driver updates its allocation bookkeeping
- **THEN** the driver may keep that CUDA VMM page mapped as an internal empty-page cache until device close
- **AND THEN** addresses in that page are still considered invalid for translated execution unless some live ELF or runtime claim exists again

### Requirement: Shared-vs-Global validity MUST remain explicit and fail-fast
The removal of runtime-visible `ELF` vs `Heap` distinction MUST NOT introduce silent fallback behavior for invalid numeric addresses.

If an ordinary translated access is neither in the supported shared window nor in a valid mapped Global range, the implementation MUST reject or trap explicitly according to the surrounding current fail-fast contract.

#### Scenario: Invalid non-shared address does not get reclassified silently
- **GIVEN** an ordinary translated access computes a numeric address outside the supported shared window and outside any valid translated Global mapping
- **WHEN** the access executes or is validated by the backend
- **THEN** the implementation surfaces an explicit failure
- **AND THEN** it does not silently reinterpret the address as belonging to some fallback ELF or heap region
