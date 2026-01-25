## 1. Instruction mapping + PTX kernel
- [x] 1.1 Write a mapping table for the exact Ventus instructions used in `testcases/simple/simple.S`.
- [x] 1.2 Confirm lane semantics: map `vid.v` to PTX `%laneid` (not `%tid.x`) and standardize launch config as one warp.
- [x] 1.3 Produce a PTX kernel implementing: `arr[lane] = arr[lane] + lane` with `base` passed as a kernel parameter.

## 2. Host runner (NVIDIA)
- [x] 2.1 Add a minimal host program that loads PTX (CUDA Driver API), finds the kernel entry, and launches it.
- [x] 2.2 Allocate a device buffer, initialize host values, copy H→D, run kernel, copy D→H.
- [x] 2.3 Validate results on host (for lane 0..31).

## 3. Build + validation
- [x] 3.1 Add a script/command to compile PTX to cubin (targeting the local GPU arch, e.g. `sm_89`) and build the host runner.
- [x] 3.2 Add a single-command smoke test that runs the runner and exits non-zero on mismatch.
- [x] 3.3 Record the exact commands used for reproducibility (nvcc/ptxas versions).

Reproducibility notes (local environment):
- nvcc: CUDA 13.1 (V13.1.80)
- ptxas: CUDA 13.1 (V13.1.80)
