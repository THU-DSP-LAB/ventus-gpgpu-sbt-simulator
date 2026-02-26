# Change: Add minimal Ventus→PTX runnable prototype (simple)

## Why
We need an end-to-end runnable artifact on NVIDIA GPU that validates the basic Ventus vector-as-SIMT semantics using a tiny program (`testcases/simple/simple.S`).

## What Changes
- Define a concrete, minimal instruction mapping table (Ventus → PTX) for the subset used by `testcases/simple/simple.S`.
- Standardize on passing the base pointer as a PTX kernel parameter (do not rely on a fixed device virtual address like `0x90000000`).
- Provide a minimal host-side runner that:
  - allocates and initializes a device buffer,
  - launches the PTX kernel with one warp (32 threads),
  - copies results back and checks correctness.

## Impact
- Affected inputs: `testcases/simple/simple.S` (semantics reference), `testcases/simple/simple.ptx` (manual kernel reference).
- New capability: a runnable prototype for correctness validation on NVIDIA GPU.
- Non-goals (this change): general Ventus ISA coverage, SIMT stack control flow (`vbranch/setrpc/join`), and automatic binary decoding/translation.
