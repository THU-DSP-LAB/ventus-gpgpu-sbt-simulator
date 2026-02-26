## ADDED Requirements

### Requirement: Accept Rodinia Ventus ELF and extract `.text` + symbols
The system SHALL accept the Ventus toolchain produced RISC-V `ELF32` inputs used by `ventus-env/rodinia/opencl/*/*.riscv`, and SHALL extract `.text` and function symbols from `.symtab` without applying relocations.

#### Scenario: Enumerate kernel entry symbols
- **WHEN** the user provides `ventus-env/rodinia/opencl/bfs/object0.riscv` as input
- **THEN** the system identifies function symbols including `BFS_1` and `BFS_2`
- **AND THEN** the system can locate each symbol start address inside `.text`

### Requirement: Decode Ventus instructions with `regext` prefix bundling
The system SHALL decode 32-bit Ventus instructions from `.text` and SHALL implement `regext` as a prefix that only affects the next instruction (bundling into a single logical decoded instruction).

#### Scenario: `regext` affects exactly one subsequent instruction
- **WHEN** decoding a Rodinia kernel that contains `regext`
- **THEN** the decoded stream contains a single logical instruction for the `regext + next` pair
- **AND THEN** the `regext` state is cleared immediately after decoding the next instruction

### Requirement: Build CFG and validate `setrpc/vbranch/join` structural constraints
The system SHALL reconstruct a basic-block CFG for each translated function and SHALL validate that every `vbranch` can be structurally lowered using the `setrpc`-derived join PC (post-dominator, single-entry, no-side-exit).

#### Scenario: Pass verification on Rodinia BFS kernels
- **WHEN** analyzing `ventus-env/rodinia/opencl/bfs/object0.riscv` for kernel functions
- **THEN** every `vbranch` in the analyzed kernels is either (a) verified as structurally reducible, or (b) the input is rejected with a clear diagnostic

### Requirement: Define supported vs unsupported features and fail fast
The system SHALL explicitly define the supported instruction subset and control-flow forms for the prototype stage, and SHALL reject unsupported inputs with clear error messages.

#### Scenario: Reject unsupported instructions or control flow
- **WHEN** the input contains `regexti` or a non-return `jalr` inside a translated function
- **THEN** the system stops translation and reports an "unsupported" diagnostic that names the instruction/form and its PC

### Requirement: Enforce `barrier` legality at converged points
The system SHALL treat Ventus `barrier` as CUDA `__syncthreads()` / PTX `bar.sync` and SHALL reject inputs where a `barrier` may execute on a diverged path.

#### Scenario: Barrier must be provably converged
- **WHEN** a function contains `barrier`
- **THEN** the system verifies the control-flow constraints that guarantee convergence at that point
- **AND THEN** translation fails with a diagnostic if convergence cannot be proven

### Requirement: Translate kernel entry functions by name and skip `_start` in prototype mode
The system SHALL translate a selected kernel entry function identified by name (e.g. from runtime `kernel_name`) using ELF `.symtab`, and SHALL NOT require translating the ELF entrypoint `_start` in prototype mode.

#### Scenario: `_start` contains an indirect jump but kernel translation still proceeds
- **WHEN** the user requests translation of `BFS_1` from `ventus-env/rodinia/opencl/bfs/object0.riscv`
- **THEN** translation starts from the `BFS_1` function symbol and its reachable callees
- **AND THEN** the existence of `jalr t1` in `_start` does not cause translation to fail
