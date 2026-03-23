# inst-support

> Status: `current`
>
> Canonical current contract for instruction support scope and semantic validation strategy.

## Purpose
Define the project's supported instruction set surface, how Spike `encoding.h` patterns are selected, and how instruction support is validated (Spike oracle vs PTX path).

## Requirements

### Requirement: Single source of truth for Spike insn whitelist
The project MUST define exactly one repository-managed whitelist file for Spike `DECLARE_INSN` IDs used as Ventus/RVV pattern inputs.

#### Scenario: Update whitelist without code changes
Given a developer wants to add a new Spike instruction pattern to the bring-up subset,  
When they edit only the whitelist file,  
Then all tools that consume Spike patterns use the updated subset without requiring changes to hardcoded lists.

### Requirement: No duplicated hardcoded want lists in tools
Tools MUST NOT carry an embedded list of Spike insn IDs for the bring-up whitelist.

#### Scenario: Prevent list drift between tools
Given `sbt_ptx`, `sbt_decode`, and `gen_spike_encoding_subset` are built from the same repository state,  
When they load Spike patterns,  
Then they must derive their wanted insn IDs from the same whitelist input and cannot diverge due to per-tool hardcoding.

### Requirement: Extend PTX emitter scalar RV32I/M coverage
The PTX backend MUST support a complete, commonly used RV32I/M scalar subset that is already decodable by the frontend, including at least:
- logic ops: `and/or/xor/andi/ori`
- shifts: `sll/srl/sra/srli/srai`
- M extension: `div/divu/rem/remu/mulh/mulhsu/mulhu`
- scalar memory: `lb/lh/lhu/sh` (in addition to existing `lw/lbu/sw/sb`)

#### Scenario: RV32I/M scalar instruction appears in a kernel
Given an input kernel whose `.text` contains RV32I/M scalar instructions from the required subset,  
When the kernel is translated to PTX with `--require-known`,  
Then translation must not fail due to missing scalar instruction lowering.

### Requirement: Extend basic vector memory ops coverage
The PTX backend MUST support the basic 12-bit offset vector memory operations:
- `vlb12.v`, `vlbu12.v`, `vlh12.v`, `vlhu12.v`
- `vsb12.v`, `vsh12.v`, `vsw12.v`

#### Scenario: Vector byte/halfword loads are used by a benchmark
Given a benchmark kernel that uses 12-bit offset vector byte/halfword loads/stores,  
When the kernel is translated to PTX and compiled by `ptxas`,  
Then translation must succeed without introducing new diagnostic subcommands or emitting a “missing instruction list” report.

### Requirement: Target instruction set is `VentusInst_basic.txt` (with explicit exceptions)
The project MUST treat the instruction table in `VentusInst_basic.txt` as the target set to be supported.

The project MUST document and enforce an explicit, small set of allowed fail-fast exceptions, including at least:
- non-`ret` forms of `jalr` (indirect jump/call)

The project MAY additionally list `regexti` as a temporary exception, provided `regext` prefix bundling remains supported in decode.

#### Scenario: Expanding support does not require duplicating lists
Given a developer expands instruction support toward `VentusInst_basic.txt`,  
When they update the whitelist, decode classification, and PTX lowering,  
Then they must not need to edit multiple drift-prone duplicated instruction lists across tools.

### Requirement: Semantic validation uses Spike oracle via OpenCL buffers
The project MUST provide a way to validate instruction semantics by comparing outputs produced via OpenCL buffers:
- the host creates input buffer A and output buffer B,
- the device program writes results into B,
- the host reads back B and compares results between Spike (Ventus PoCL device) and the PTX path.

For floating-point operations, the comparison MUST allow approximate equality (tolerance-based) rather than requiring bit-identical results.

#### Scenario: Micro-test compares Spike vs PTX outputs
Given a micro-test device program that reads inputs from buffer A and writes results into buffer B,  
When the same program is executed via OpenCL on the Spike device and the PTX device,  
Then the host-side comparison of buffer B must pass (exact match for integer data and tolerance-based match for floating-point data).

### Requirement: Decode scalar F-extension mnemonics as Zfinx-style X-reg ops
Ventus scalar ISA is close to `RV32IMA_zicsr_zfinx`. For this project, scalar single-precision floating-point
instructions listed in `VentusInst_basic.txt` MUST be treated as operating on the scalar **X register file** (Zfinx
model): values are carried as `f32` raw bits in `x0..x31`.

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
- execute each scalar F-extension mnemonic listed above at least once (no dead-only coverage-only blocks), and
- make the instruction effects observable via OpenCL output buffers so the host can compare Spike vs PTX outputs.

For float results, the comparison MUST use tolerance-based equality (atol/rtol). For integer results (e.g. `feq_s`,
`flt_s`, `fle_s`, `fclass_s`), the comparison MUST match exactly.

#### Scenario: Coverage gate remains full with updated target table
- **WHEN** `tools/microtest_coverage_gate.sh --require-full` is executed against the updated `VentusInst_basic.txt`
- **THEN** it MUST report full mnemonic coverage for `(VentusInst_basic - inst_exceptions)` including the scalar F
  mnemonics added by this change
