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

### Requirement: Repository-local custom decode coexists with the Spike-backed pattern subset
The project MUST allow custom instruction families that are absent from Spike `encoding.h` to be recognized through a repository-local opcode/funct decoder path.

The build-time Spike subset remains the single source of truth for Spike-backed pattern inputs, but it MUST NOT be treated as the only decode entrypoint for future custom instruction families.

#### Scenario: Custom instruction is not declared in Spike encoding
- **GIVEN** an input ELF contains a project-supported custom non-MMA instruction that is absent from `../spike/riscv/encoding.h`
- **WHEN** `sbt_decode decode --require-known` or `sbt_ptx --require-known` is executed
- **THEN** the instruction may still be recognized through the repository-local custom decode path
- **AND THEN** translation does not fail merely because the instruction is missing from Spike `DECLARE_INSN(...)`

### Requirement: Custom non-MMA instructions preserve ordinary vector-register semantics
For the supported custom non-MMA instruction families, `v0` MUST remain an ordinary vector register.

The project MUST NOT introduce implicit RVV mask semantics or a special `v0` mask-register interpretation for these instruction families. Any encoded `vm`/`m` bit in the current non-MMA custom instruction families MUST be treated as encoding metadata only: it MUST NOT cause execution to read `v0`, derive a separate write mask, or bypass the SIMT-stack-controlled active mask model.

#### Scenario: Custom non-MMA operands are decoded and lowered
- **GIVEN** a kernel uses supported custom `shuffle`, `vcvt`, packed arithmetic, or SFU instructions
- **WHEN** the instructions are decoded and lowered
- **THEN** operand fetch and writeback use ordinary vector-register semantics
- **AND THEN** translation does not read an implicit mask value from `v0`
- **AND THEN** any encoded `vm`/`m` bit does not enable a separate `v0`-based mask path

### Requirement: Custom non-MMA support is compile-supported on the shared `sm_89` baseline
The project MUST compile-support the current non-MMA custom instruction families under `--require-known`, including:
- `shuffle.idx/up/down/bfly`
- `vcvt.f32.fp16`, `vcvt.f16.fp32`, `vcvt.fp32.bf16`, `vcvt.bf16.fp32`
- packed `f16x2` and `bf16x2` add/mul/fma
- `fp32` SFU (`ex2/lg2/rcp/sqrt/rsqrt/sin/cos/tanh/gelu/silu`)
- packed `f16x2` SFU
- packed `bf16x2` SFU

The generated PTX for this support surface MUST target the shared custom-support baseline `.version 7.8` / `.target sm_89`. Where PTX lacks a direct native instruction, the backend MAY lower the instruction via explicit helper sequences or unpack/compute/repack logic, but it MUST remain compile-supported on `sm_89`.

#### Scenario: Non-MMA custom kernel translates under `--require-known`
- **GIVEN** an input kernel uses only the supported non-MMA custom instruction families
- **WHEN** `sbt_ptx --require-known --sm 89` translates the kernel
- **THEN** PTX emission does not fail with `unknown instruction` or `unsupported.inst` for those instruction families
- **AND THEN** the generated PTX compiles with `ptxas -arch=sm_89`

### Requirement: Packed custom instructions follow the 32-bit container contract
For packed non-MMA custom instruction families (`f16x2` / `bf16x2` arithmetic and SFU), the project MUST treat one active vector element as one 32-bit packed container.

The canonical contract is:
- `vl` counts packed containers, not individual 16-bit halves
- legality is not defined as ordinary `vsew=16` element-wise execution
- packed `bf16x2` support on the shared `sm_89` baseline may mix native PTX and explicit composite lowering, but it MUST remain part of the supported non-MMA family set

#### Scenario: Packed custom microtest interprets `vl`
- **GIVEN** a packed `f16x2` or `bf16x2` custom kernel is executed
- **WHEN** the kernel consumes `vl` active elements
- **THEN** the implementation interprets those elements as `vl` packed 32-bit containers
- **AND THEN** validation and lowering do not reinterpret the instruction as two independent ordinary 16-bit vector elements per lane

### Requirement: Current non-MMA custom semantic validation uses Spike-backed OpenCL comparison
For the current supported non-MMA custom instruction families, the canonical semantic validation path MUST compare observable OpenCL output buffers produced by the Spike backend and the PTX backend.

Compile-first success is required but is not sufficient by itself.

#### Scenario: Non-MMA custom oracle run compares Spike and PTX
- **GIVEN** a supported non-MMA custom microtest kernel
- **WHEN** the project runs its semantic validation gate
- **THEN** the gate executes the same kernel through both the Spike backend and the PTX backend
- **AND THEN** it compares observable output buffers with exact comparison for integer/packed arithmetic cases and tolerance-based comparison for floating-point SFU cases

### Requirement: Current contract supports the landed first-batch MMA subset on `sm_89`
The current landed custom instruction support surface MUST include the committed `row.col` MMA subset below on the shared `.version 7.8` / `.target sm_89` baseline:

- direct-native:
  - `m16n8k16 row.col f16.f16.f16.f16`
  - `m16n8k16 row.col f32.f16.f16.f32`
  - `m16n8k16 row.col f32.bf16.bf16.f32`
  - `m16n8k8 row.col f32.tf32.tf32.f32`
- committed `split-n` composite:
  - `m16n16k16 row.col f16.f16.f16.f16`
  - `m16n16k16 row.col f32.f16.f16.f32`
  - `m16n16k16 row.col f32.bf16.bf16.f32`
  - `m16n16k8 row.col f32.tf32.tf32.f32`

For this landed subset, the project MUST:
- decode the MMA instruction as known through dedicated MMA metadata,
- emit PTX through the documented native/composite lowering path,
- pass compile-first validation with `ptxas -arch=sm_89`,
- and pass semantic validation using observable outputs.

For the current supported `fp16 -> fp16` families, semantic validation MUST compare observable PTX and Spike outputs against a CPU reference with an explicit fp16-tolerant comparison rule:

- finite results use `<= 1 fp16 ULP` or an equivalent documented host-side tolerance,
- `NaN` results compare by classification rather than payload,
- and integer / structural metadata outputs remain exact-match only.

#### Scenario: Landed MMA kernel translates and passes the oracle
- **GIVEN** an input kernel uses only the landed current MMA subset
- **WHEN** `sbt_ptx --require-known --sm 89` translates it and the MMA oracle gate is executed
- **THEN** decode and PTX emission succeed
- **AND THEN** `ptxas -arch=sm_89` succeeds
- **AND THEN** the observable Spike and PTX outputs match the CPU reference under the documented MMA comparison rules

### Requirement: Unsupported or blocked MMA combinations still fail explicitly
The current landed MMA support MUST remain limited to the first-batch subset above.

Within the `fp16 -> fp16` family, only the following combinations are current supported behavior:

- direct-native `m16n8k16 row.col f16.f16.f16.f16`
- committed `split-n` composite `m16n16k16 row.col f16.f16.f16.f16`

Any other deferred/research MMA family, any non-`row.col` MMA family, and any other `fp16 -> fp16` combination MUST continue to fail explicitly under `--require-known`.

An unsupported `fp16 -> fp16` path MAY still decode as known through dedicated MMA metadata, but it MUST fail explicitly before PTX compile-first or semantic validation proceeds; the backend MUST NOT silently reinterpret it as one of the landed current families.

#### Scenario: Unsupported or blocked MMA path does not silently lower
- **GIVEN** an input kernel includes an MMA combination outside the landed current subset, or an `fp16 -> fp16` family outside the two current supported combinations
- **WHEN** `sbt_decode --require-known` or `sbt_ptx --require-known` is executed for a deferred/research/non-`row.col` family, or `sbt_ptx --require-known` enters lowering for a non-current `fp16 -> fp16` family
- **THEN** translation or lowering fails explicitly (`unknown` / `unsupported` / blocked diagnostic)
- **AND THEN** the current `inst-support` contract keeps the landed MMA subset and the remaining blocked/deferred/research MMA work clearly separated

### Requirement: Current fp16-to-fp16 native MMA lowering uses an explicitly validated direct window-to-tuple contract
For `m16n8k16 row.col f16.f16.f16.f16`, the PTX backend MUST instantiate native operand tuples through an explicit repository-local contract that is validated against:

- Spike-equivalent `VGPR window -> logical tile` semantics,
- a CPU reference,
- and a handwritten-PTX probe for the same native PTX form.

The implementation MUST remain consistent with the layered model in `doc/mma/LOWERING_ARCHITECTURE.md` at the semantic level:

- `VGPR window`
- `logical coordinates`
- `PTX native fragment tuple`

The implementation MUST NOT require a materialized logical-tile buffer as an intermediate result. Instead, it MAY construct PTX tuples directly from the Ventus source windows, provided that the tuple element coordinates are derived from an explicit Spike-equivalent logical-coordinate contract rather than guessed from raw VGPR window order.

#### Scenario: Supported native fp16-to-fp16 path does not guess tuple order
- **GIVEN** `m16n8k16 row.col f16.f16.f16.f16` is lowered on the current `sm_89` baseline
- **WHEN** the emitter constructs PTX operand tuples
- **THEN** it uses an explicit validated direct window-to-tuple contract derived from the repository-local MMA ABI descriptors
- **AND THEN** the lowering does not guess PTX tuple order directly from raw Ventus register-window order alone

### Requirement: Current fp16-to-fp16 split-n lowering reuses the validated native building block
For `m16n16k16 row.col f16.f16.f16.f16`, the PTX backend MUST lower one Ventus instruction into exactly two native `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16` sub-operations over disjoint logical `n` slices.

The composite contract is:

- sub-op 0 covers logical `n = [0, 7]`,
- sub-op 1 covers logical `n = [8, 15]`,
- both sub-ops share the same logical `A` coordinate space,
- and `B/C/D` tuple construction and merge steps follow the same explicit ABI-descriptor contract as the native `m16n8k16` family.

#### Scenario: Supported split-n fp16-to-fp16 path rebuilds the full logical tile
- **GIVEN** `m16n16k16 row.col f16.f16.f16.f16` is lowered on the current `sm_89` baseline
- **WHEN** the PTX backend emits the composite lowering
- **THEN** it emits exactly two native `m16n8k16 row.col f16/f16/f16/f16` sub-operations
- **AND THEN** the combined writeback reconstructs the original logical `16 x 16` output tile without overlap or silent truncation
