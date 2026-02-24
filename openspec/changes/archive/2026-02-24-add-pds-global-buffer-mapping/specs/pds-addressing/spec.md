## ADDED Requirements

### Requirement: Runtime SHALL provide PDS parameters to the PTX kernel
The translated PTX kernel ABI MUST include runtime-provided PDS parameters:
- `pds_base_vaddr` (u32 Ventus numeric address of the global PDS buffer base)
- `pds_size_per_thread` (u32 bytes of private memory per thread)

#### Scenario: Kernel launch provides PDS base and size
- **WHEN** the driver launches a translated PTX kernel for a Ventus ELF
- **THEN** it MUST pass `pds_base_vaddr` and `pds_size_per_thread` in the kernel parameter list in the defined order

### Requirement: The PTX backend MUST compute CSR_PDS per warp using the software-stack formula
The PTX backend MUST compute `CSR_PDS` (warp base) as:
- `blk_linear = ctaid.x + nctaid.x * (ctaid.y + nctaid.y * ctaid.z)`
- `warp_linear = blk_linear * warps_per_block + warp_id_in_block`
- `CSR_PDS = pds_base_vaddr + warp_linear * (NUMT * pds_size_per_thread)` where `NUMT = 32`

#### Scenario: CSR_PDS read returns computed warp base
- **WHEN** a kernel executes `csrrs rd, CSR_PDS(x807), x0`
- **THEN** the backend MUST produce a value equivalent to the computed `CSR_PDS` for the executing warp

### Requirement: `vlw.v/vsw.v` MUST access the global PDS buffer via numeric address mapping
The PTX backend MUST lower `vlw.v` and `vsw.v` to global-memory accesses backed by the runtime PDS buffer (not PTX local memory), using the numeric address mapping used by other loads/stores.

For each lane:
- `base_addr = vs1 + simm11` (byte offset)
- `addr = CSR_PDS + ((base_addr & ~3) * NUMT) + (laneid << 2)` where `NUMT = 32`
- `vlw.v` loads/stores u32 at `addr`

#### Scenario: PDS indexed access shares backing with ordinary accesses
- **WHEN** `vlw.v/vsw.v` compute an address inside the global PDS buffer region
- **THEN** the resulting load/store MUST target the same global backing as any ordinary load/store to the same numeric address

### Requirement: PTX local PDS emulation MUST NOT be used
The PTX backend MUST NOT emulate PDS using per-thread PTX `.local` arrays as a substitute for the runtime global PDS buffer.

#### Scenario: Translation does not allocate `__sbt_pds` local backing
- **WHEN** a kernel containing `vlw.v/vsw.v` is translated to PTX
- **THEN** the emitted PTX MUST NOT declare or use a `.local __sbt_pds[]` backing store for PDS semantics

