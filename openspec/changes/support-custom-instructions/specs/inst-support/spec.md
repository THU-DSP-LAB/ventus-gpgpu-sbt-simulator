# inst-support (delta spec)

## ADDED Requirements

### Requirement: Custom instructions may use a repository-local decode path outside Spike patterns
The project MUST allow custom instructions that are absent from Spike `encoding.h` to be decoded by a repository-local opcode/funct decoder path, without requiring those instructions to be present in `data/spike_want.txt`.

The existing Spike whitelist remains the single source of truth for **Spike-backed** pattern inputs, but it MUST NOT be treated as the only possible decode entrypoint for future custom instruction families.

#### Scenario: Custom instruction is not declared in Spike encoding
- **GIVEN** an input ELF contains a custom instruction defined by the project but absent from `../spike/riscv/encoding.h`
- **WHEN** `sbt_decode decode --require-known` or `sbt_ptx --require-known` is executed
- **THEN** the instruction may still be recognized through the repository-local custom decode path
- **AND THEN** translation must not fail merely because the instruction is missing from Spike `DECLARE_INSN(...)`

### Requirement: Custom non-MMA instructions preserve ordinary vector-register semantics
For the custom non-MMA instruction families introduced by this change, `v0` MUST remain an ordinary vector register.

The project MUST NOT introduce implicit RVV mask semantics or a special `v0` mask register interpretation for these instruction families. Any encoding bit previously described as `vm`/`m` in pre-canonical notes MUST be canonically defined before implementation, and MUST NOT silently reintroduce `v0`-as-mask behavior.

For the current Ventus ISA contract, any `vm`/`m` bit carried by these non-MMA custom encodings MUST be treated as a non-mask encoding bit only: it MUST NOT cause execution to read `v0`, derive a separate write mask, or bypass the SIMT-stack-controlled active mask model.

#### Scenario: Non-MMA custom instruction reads operands
- **GIVEN** a kernel uses custom `shuffle`, `vcvt`, packed arithmetic, or SFU instructions
- **WHEN** the instructions are decoded and lowered
- **THEN** operand fetch and writeback use ordinary vector-register semantics
- **AND THEN** translation does not read an implicit mask value from `v0`
- **AND THEN** any encoded `vm`/`m` bit does not enable a separate `v0`-based mask path

### Requirement: Custom-support PTX baseline follows the shared active baseline before implementation
Before implementation begins for the active custom-instruction changes, the project MUST freeze a shared active baseline document that records a single project-wide PTX `.version` / `.target sm_XX` baseline for the current supported custom subset.

The project MUST NOT let parallel active changes implicitly choose different baselines.

#### Scenario: Baseline decision happens before custom implementation
- **GIVEN** the project is preparing to implement the active custom-instruction changes
- **WHEN** the team freezes the shared active baseline for packed non-MMA instructions and candidate MMA support
- **THEN** the required PTX `.version` / `.target sm_XX` is decided as a single project-wide baseline before implementation proceeds
- **AND THEN** the project does not allow one active change to land on top of one baseline while another later forces a second incompatible baseline bump
- **AND THEN** the integrated runtime path is migrated together with that baseline, rather than continuing to default to an older hidden SM target

### Requirement: This change supports all non-MMA custom instruction families
This change MUST cover every custom instruction family in `doc/CUSTOM_INSTRUCTION_INPUT.md` except MMA, including at least:

- `shuffle.idx`
- `shuffle.up`
- `shuffle.down`
- `shuffle.bfly`
- `vcvt.f32.fp16`
- `vcvt.f16.fp32`
- `vcvt.fp32.bf16`
- `vcvt.bf16.fp32`
- packed `f16x2` add/mul/fma
- packed `bf16x2` add/mul/fma
- `fp32` SFU (`ex2/lg2/rcp/sqrt/rsqrt/sin/cos/tanh/gelu/silu`)
- packed `f16x2` SFU
- packed `bf16x2` SFU

These instructions MUST be compile-supported under `--require-known`; where PTX does not provide a direct native instruction, the backend MAY lower them via explicit helper sequences, unpack/compute/repack logic, or equivalent explicit lowering.

For packed `bf16x2` families on a shared baseline below `sm_90`, the active artifacts MUST explicitly document the chosen mixed native/composite lowering path rather than assuming that native packed BF16 arithmetic or SFU opcodes are available for every operation.

#### Scenario: Non-MMA custom kernel compiles
- **GIVEN** an input kernel uses only the non-MMA custom instruction families listed above
- **WHEN** `sbt_ptx --require-known` translates the kernel
- **THEN** PTX emission must not fail with `unknown instruction` or `unsupported.inst` for those instruction families
- **AND THEN** the generated PTX must compile with `ptxas` for the chosen project-wide custom-support baseline

#### Scenario: Packed bf16x2 support stays available on the shared `sm_89` baseline
- **GIVEN** the shared active baseline is frozen to `.version 7.8` / `sm_89`
- **WHEN** the project lowers packed `bf16x2` arithmetic or SFU instructions whose direct PTX native forms are unavailable below `sm_90`
- **THEN** the active artifacts document an explicit mixed native/composite lowering path
- **AND THEN** `bf16x2` support remains part of the supported non-MMA family set rather than being silently deferred or dropped

### Requirement: Packed custom instructions define their `vtype` contract explicitly before implementation
For packed non-MMA custom instruction families (`f16x2` / `bf16x2` arithmetic and SFU), the project MUST define how the instructions relate to the current Ventus `vtype` contract before implementation.

The canonical contract MUST state at least:
- whether execution is defined in terms of 32-bit containers, `vsew`, or both,
- what operand/result register grouping assumptions are legal,
- and how `vl` interacts with packed-lane execution.

#### Scenario: Packed instruction legality is reviewed
- **GIVEN** a developer is implementing or testing packed `f16x2` / `bf16x2` instructions
- **WHEN** they consult the canonical contract
- **THEN** the legality and execution model relative to `vtype/vsew/vlmul/vl` is explicit
- **AND THEN** decoder, pretty-print, lowering, and microtests do not need to guess that contract independently

### Requirement: Non-MMA custom semantics are validated by an explicitly chosen oracle path
The project MUST provide microtests that make this change's non-MMA custom instruction effects observable through OpenCL buffers and compare outputs against an explicitly chosen oracle path.

Before implementation, the active artifacts MUST name which oracle path is used for these instructions in the shared active baseline document (for example Spike-backed oracle if upstream support exists, or a repository-managed reference-oracle path if it does not).

For floating-point outputs, comparison MUST allow approximate equality. For integer / packed-bit results, comparison MUST be exact unless the relevant spec explicitly permits tolerance.

#### Scenario: Non-MMA custom instruction microtests are observable
- **GIVEN** a microtest kernel exercises a Phase 1 custom instruction family
- **WHEN** the kernel is executed through the semantic validation path
- **THEN** the instruction's result is written to observable output buffers
- **AND THEN** the validation run fails explicitly on mismatches rather than silently skipping the instruction family
