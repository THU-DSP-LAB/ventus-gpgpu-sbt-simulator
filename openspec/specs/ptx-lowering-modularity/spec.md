# ptx-lowering-modularity Specification

## Purpose
定义当前 PTX emitter 的模块化结构合同，明确 shared emission core、domain-specific lowering units、instruction dispatcher precedence 与结构化验证覆盖面的 current 要求。
## Requirements
### Requirement: PTX emitter SHALL separate shared emission core from domain-specific lowering units
The PTX emitter implementation MUST maintain a clear boundary between:

- shared emission core responsibilities
- domain-specific lowering responsibilities

The shared emission core is the authority for function/module assembly and shared lowering contract. It MUST own at least:

- PTX module/function header and body assembly
- fixed register and blob ABI contract
- temporary-register and label allocation
- shared address-mapping helpers
- entry/helper prologue and epilogue logic
- CFG block traversal and fallthrough emission

Domain-specific lowering units MUST consume this shared core contract rather than each redefining their own copies of these responsibilities.

#### Scenario: Scalar lowering does not redefine module assembly
- **GIVEN** the emitter lowers a current supported scalar instruction
- **WHEN** the scalar lowering path emits PTX
- **THEN** module/function body assembly, temporary allocation, and blob ABI semantics still come from the shared emission core
- **AND THEN** the scalar lowering unit does not carry its own duplicate module-assembly or ABI contract

### Requirement: Lowering-unit boundaries SHALL follow semantic authority rather than file-size goals
When splitting PTX lowering into multiple implementation units, the boundary MUST be chosen by semantic lowering authority and shared contract, not by line-count reduction alone.

At minimum, the modularized design MUST preserve coherent ownership for these domains:

- control-flow and structured-control lowering, including direct-call and builtin-call resolution
- scalar memory / scalar integer / scalar floating-point lowering
- vector memory / vector register / vector integer / vector floating-point / compare / convert / mask lowering
- custom non-MMA lowering
- MMA PTX materialization

The implementation MUST NOT split tightly coupled lowering logic into separate units solely to reduce file size if doing so would duplicate shared helpers, duplicate contract checks, or obscure the true ownership boundary.

#### Scenario: Builtin call lowering remains in the control/call domain
- **GIVEN** an inlined builtin call eventually produces scalar, vector, or math-like PTX instructions
- **WHEN** the modularized emitter assigns ownership of that lowering path
- **THEN** the ownership remains attached to the direct-call/control resolution domain that decides the callee behavior
- **AND THEN** the implementation does not move it to another unit solely because the resulting PTX looks like scalar/vector math

### Requirement: Instruction dispatch SHALL be a thin domain dispatcher with explicit precedence
The instruction-level PTX emission entrypoint MUST become a thin dispatcher that selects lowering by explicit semantic domain precedence rather than by a single monolithic inlined branch chain.

The precedence between domain handlers MUST remain explicit and reviewable as part of the emitter contract.

The dispatcher MAY short-circuit through domain-specific `try_emit_*` style helpers, but correctness MUST NOT depend on an implicit ordering hidden inside one large mixed-domain function body.

The dispatcher/core boundary MUST remain the central authority for shared preconditions that apply before domain-specific lowering begins. These shared preconditions include comment emission and any repository-managed fail-fast validation that is intended to run before domain dispatch on the current supported path, such as scalar-execution classification metadata gates when required.

Domain-specific lowering units MUST NOT silently re-scatter these shared preconditions into per-domain copies if doing so would obscure the single reviewable entrypoint for current supported-path dispatch.

#### Scenario: Domain precedence is visible without reading every lowering branch
- **GIVEN** a maintainer needs to confirm whether a current supported instruction is handled by control, scalar, vector, custom, or MMA lowering
- **WHEN** they inspect the instruction-dispatch entrypoint
- **THEN** the domain ordering is visible directly in the dispatcher structure
- **AND THEN** they do not need to scan a monolithic mixed-domain branch chain to infer the precedence contract

### Requirement: Structural validation SHALL cover all emitter lowering translation units
The project's structural validation for PTX lowering authority and name-usage restrictions MUST remain effective after emitter modularization.

If current validation or static checks previously assumed a single `sbt/ptx_emit.cpp`, they MUST be updated so that:

- supported-path authority checks still cover every lowering translation unit
- allowed external/comment/diagnostic uses of mnemonic names remain explicitly scoped
- external mnemonic contract regressions still cover comments / diagnostics / external builtin symbol behavior after the split
- compile-first and relevant emitter regressions continue to validate the modularized implementation as one coherent emitter

Emitter modularization MUST NOT reduce validation strength by leaving newly introduced lowering units outside the existing structural checks.

#### Scenario: Name-allowlist validation still covers modularized lowering files
- **GIVEN** the emitter has been split into multiple lowering implementation files
- **WHEN** structural validation checks mnemonic-name usage on the PTX lowering path
- **THEN** the check covers the full modularized emitter file set
- **AND THEN** it does not silently stop enforcing the authority boundary for files moved out of the original monolithic source
