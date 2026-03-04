# inst-support (delta spec)

## ADDED Requirements

### Requirement: Decode scalar F-extension mnemonics as Zfinx-style X-reg ops
Ventus scalar ISA is close to `RV32IMA_zicsr_zfinx`. For the purposes of this project, scalar single-precision
floating-point instructions listed in `VentusInst_basic.txt` MUST be treated as operating on the scalar **X register
file** (Zfinx model): values are carried as `f32` raw bits in `x0..x31`.

The decoder MUST recognize the following mnemonics (normalized to use `_` instead of `.`) as **known** instructions and
extract their operands/immediates according to the RISC-V encodings:
- `fmadd_s`, `fmsub_s`, `fnmsub_s`, `fnmadd_s`
- `fadd_s`, `fsub_s`, `fmul_s`, `fdiv_s`, `fsqrt_s`
- `fsgnj_s`, `fsgnjn_s`, `fsgnjx_s`
- `fmin_s`, `fmax_s`
- `fcvt_w_s`, `fcvt_wu_s`, `fcvt_s_w`, `fcvt_s_wu`
- `feq_s`, `flt_s`, `fle_s`
- `fclass_s`
- `flw`, `fsw`
- `fmv_w_x`, `fmv_x_w`

#### Scenario: Decoder accepts F mnemonics under `--require-known`
- **WHEN** `sbt_decode decode --require-known` is executed on a kernel whose `.text` contains any of the mnemonics above
- **THEN** decoding MUST NOT fail with `unknown instruction`, and the decoded JSON MUST contain normalized names for them

### Requirement: PTX lowering for scalar F-extension mnemonics (compile-supported)
The PTX backend MUST lower the scalar F-extension mnemonics listed above such that the output is **compile-supported**:
decode + CFG verify + PTX emit + `ptxas` compilation succeed under `--require-known`.

#### Scenario: Kernel containing F mnemonics translates and compiles
- **WHEN** `sbt_ptx --require-known` translates a kernel that contains any of the listed scalar F mnemonics
- **THEN** PTX emission MUST NOT fail with `unsupported.inst` due to those mnemonics, and the generated PTX MUST compile
  with `ptxas` for the configured SM target

### Requirement: Micro-test semantic validation for scalar F-extension mnemonics
The project MUST provide micro-tests that:
- execute each scalar F-extension mnemonic listed above at least once (no dead-only “coverage-only” blocks), and
- make the instruction effects observable via OpenCL output buffers so the host can compare Spike vs PTX outputs.

For float results, the comparison MUST use tolerance-based equality (atol/rtol). For integer results (e.g. `feq_s`,
`flt_s`, `fle_s`, `fclass_s`), the comparison MUST match exactly.

#### Scenario: Coverage gate remains full with updated target table
- **WHEN** `tools/microtest_coverage_gate.sh --require-full` is executed against the updated `VentusInst_basic.txt`
- **THEN** it MUST report full mnemonic coverage for `(VentusInst_basic - inst_exceptions)` including the scalar F
  mnemonics added by this change

## REMOVED Requirements
None.

