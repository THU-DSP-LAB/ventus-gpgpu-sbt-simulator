## Implementation Tasks

- [x] Extend decode IR to carry FP rounding mode used by scalar F instructions (e.g. add `FpRoundingMode` + `DecodedInst::fp_rm`) in `sbt/riscv_decode.hpp`.
- [x] Implement scalar decode for F-extension mnemonics in `sbt/riscv_decode.cpp` (Zfinx model: use X regs):
- [x] Decode `flw/fsw` (opcodes `0x07/0x27`) with `i12/s12` immediates and normalized names `flw/fsw`.
- [x] Decode `fmadd_s/fmsub_s/fnmsub_s/fnmadd_s` (R4-type opcodes) with `rs3` and `rm` extraction.
- [x] Decode `fadd_s/fsub_s/fmul_s/fdiv_s/fsqrt_s`, `fsgnj_*`, `fmin_s/fmax_s`, `feq_s/flt_s/fle_s`, `fclass_s`, `fcvt_*`, `fmv_w_x/fmv_x_w` (opcode `0x53`) with correct operand classes and `rm` extraction where applicable.
- [x] Ensure name normalization matches coverage tooling (`.` -> `_`) and that `sbt_decode --require-known` accepts these mnemonics.

- [x] Add PTX lowering for the new scalar float mnemonics in `sbt/ptx_emit.cpp` (uniform scalar path):
- [x] Add helper to map `FpRoundingMode` to PTX modifiers for f32 ops (`.rn/.rz/.rm/.rp`) and integer conversion variants (`cvt.rni/rzi/rmi/rpi`).
- [x] Lower `flw/fsw` via existing scalar address mapping as u32 load/store (bit-preserving).
- [x] Lower `fadd_s/fsub_s/fmul_s/fdiv_s/fsqrt_s` using `%f` temporaries + `mov.b32` bitcasts and store raw bits back to X regs.
- [x] Lower FMA family (`fmadd_s/fmsub_s/fnmsub_s/fnmadd_s`) using `fma.<rm>.f32` plus sign transforms.
- [x] Lower `fsgnj_s/fsgnjn_s/fsgnjx_s` via integer-domain sign-bit manipulation on raw bits.
- [x] Lower `fmin_s/fmax_s` with explicit NaN handling (and document/cover `-0/+0` behavior with microtests).
- [x] Lower `feq_s/flt_s/fle_s` to exact 0/1 integer results in X regs.
- [x] Lower `fcvt_w_s/fcvt_wu_s/fcvt_s_w/fcvt_s_wu` with rounding-mode aware PTX `cvt` instructions.
- [x] Lower `fclass_s` by computing the 10-bit IEEE-754 class mask; add microtests for NaN/Inf/zero/subnormal cases within practical limits.
- [x] Handle `rm=DYN` explicitly as documented in design; if `rm=RMM` is unsupported, fail-fast with an explicit error that includes `pc` and `rm`.

- [x] Add/extend microtests to cover each new scalar mnemonic in `testcases/ocl_compare/kernels.cl`:
- [x] Add at least one kernel that exercises arithmetic + FMA + sign-injection + min/max and writes float outputs (compared with atol/rtol).
- [x] Add kernels that exercise `fcvt_*` with representative values and compare integer/float outputs appropriately.
- [x] Add kernels that exercise `feq_s/flt_s/fle_s` and `fclass_s` and write integer outputs (exact compare).
- [x] Add a kernel that exercises `flw/fsw` and `fmv_w_x/fmv_x_w` (bit-preserving behavior).

- [x] Update the default kernel lists in `tools/ventus_ocl_compare.py`:
- [x] Include the new scalar-float microtest kernels in `--kernels` defaults so `tools/microtest_coverage_gate.sh` exercises them.
- [x] Add float-output scalar-float kernels to `--float-kernels` defaults so outputs are compared with tolerance.

- [x] Verify coverage gate and regressions:
- [x] Run `tools/microtest_coverage_gate.sh` and confirm full coverage for `(VentusInst_basic - data/inst_exceptions.txt)` with updated `VentusInst_basic.txt`.
- [x] Run `tools/regress.sh --preset quick --arch sm_75` and confirm no new failures.

- [x] Documentation sync:
- [x] Update `README.md` and/or `doc/IMPLEMENTATION_CODEMAP.md` to mention scalar float (zfinx) instruction support and the current rounding/CSR limitations (if any).
