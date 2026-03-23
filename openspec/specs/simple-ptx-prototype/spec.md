# simple-ptx-prototype

> Status: `legacy`
>
> This spec is kept only as early prototype background. `testcases/simple/` is no longer the current implementation or regression baseline.

## Purpose
Preserve the minimal runnable Ventus-to-PTX prototype requirements used for early one-warp semantic validation.

## Requirements

### Requirement: Runnable simple Ventus→PTX prototype
The system SHALL provide a minimal runnable NVIDIA GPU prototype for the Ventus instruction subset used by `testcases/simple/simple.S`, using a PTX kernel with a base pointer passed via kernel parameters.

#### Scenario: Run one-warp kernel and validate memory
- **WHEN** the host allocates a device buffer of at least 32 x 4 bytes and initializes it with known 32-bit values
- **AND WHEN** the host launches the PTX kernel with exactly one warp (1 block, 32 threads) passing the device pointer as `base`
- **THEN** for every lane `i` in [0, 31], the output at `base + 4*i` equals `input[i] + i`

### Requirement: Defined instruction mapping table (simple subset)
The system SHALL document a precise Ventus→PTX mapping for the instructions used by `testcases/simple/simple.S`.

#### Scenario: Map lane id and memory operations
- **WHEN** translating `vid.v`, `vlw12.v`, and `vsw12.v` from the simple testcase
- **THEN** the translation uses PTX `%laneid` for lane id and `ld.global.u32`/`st.global.u32` for 32-bit memory operations
