## MODIFIED Requirements

### Requirement: Current supported control-flow consumers SHALL consume explicit decode-produced control semantics
For the current supported control-flow instruction subset, the repository MUST treat decode-produced structured control semantics as the authority for all main-pipeline control-flow consumers, not only for PTX emission.

The current contract is:

- decode / shared metadata produce the structured control-flow semantics before downstream control-flow decisions are made
- `sbt/cfg.cpp` consumes that structured authority for leader detection, terminator classification, and edge construction
- `sbt/cfg_verify.cpp` consumes that structured authority for `setrpc` / `vbranch` / `join` / `barrier` / unsupported `jalr` analysis
- `tools/sbt_ptx.cpp` consumes that structured authority when scanning direct-call callees
- `sbt/ptx_emit.cpp` continues to consume that same authority for supported-path control lowering

For the current supported path, these downstream consumers MUST NOT reconstruct control-flow meaning by branching on `DecodedInst.name`.

The structured authority MUST cover at least:

- scalar branch vs vector branch
- direct jump vs direct call
- return vs indirect terminator
- structured control kinds such as `setrpc`, `join`, `barrier`, and `endprg`
- ordinary metadata needed by control analysis helpers such as identifying `auipc` when resolving current `setrpc` join targets

#### Scenario: CFG build follows decode-produced control semantics
- **GIVEN** a current supported control-flow instruction reaches CFG build
- **WHEN** `sbt/cfg.cpp` classifies leaders, terminators, and outgoing edges
- **THEN** it uses decode-produced structured control semantics as the authority
- **AND THEN** it does not choose branch/jump/call/return meaning by matching `DecodedInst.name`

#### Scenario: CFG verify follows decode-produced control semantics
- **GIVEN** a current supported control-flow instruction reaches CFG verification
- **WHEN** `sbt/cfg_verify.cpp` analyzes `setrpc`, `vbranch`, `join`, `barrier`, or unsupported `jalr`
- **THEN** it uses the same structured control semantics consumed by the rest of the main pipeline
- **AND THEN** it does not recover control-flow meaning from mnemonic text

#### Scenario: Direct-call closure scan follows decode-produced call semantics
- **GIVEN** a current supported direct call reaches the `sbt_ptx` call-graph closure scan
- **WHEN** the tool identifies direct callees
- **THEN** it uses decode-produced direct-call semantics rather than testing whether the mnemonic string equals `jal`

### Requirement: Missing control-semantics authority SHALL fail explicitly on the supported main pipeline
If a current supported control-flow instruction reaches CFG build, CFG verify, or direct-call closure scanning without the control semantics required by that consumer, the implementation MUST fail explicitly rather than falling back to mnemonic parsing.

#### Scenario: Supported control-flow consumer does not fall back to name parsing
- **GIVEN** a current supported control-flow instruction reaches CFG build, CFG verify, or direct-call scanning
- **AND GIVEN** its required structured control semantics are missing, incomplete, or inconsistent with the supported contract
- **WHEN** the consumer attempts to classify the instruction
- **THEN** the implementation fails explicitly
- **AND THEN** it does not recover by parsing `DecodedInst.name`

## ADDED Requirements

### Requirement: External mnemonic contract SHALL remain non-authoritative for supported control-flow consumers
`DecodedInst.name` SHALL remain available for external-facing decode/reporting use, but it SHALL NOT remain the semantic authority for current supported control-flow consumers after control-semantics migration.

The current external mnemonic contract still includes at least:

- pretty output
- JSON / diagnostics
- coverage / mnemonic reporting
- human-readable verification or error-reporting fields where the mnemonic is diagnostic text rather than the authority

#### Scenario: Poisoned mnemonic does not alter supported control-flow behavior
- **GIVEN** a current supported control-flow instruction has valid structured control semantics
- **AND GIVEN** a regression test intentionally replaces `DecodedInst.name` with an unrelated poison string
- **WHEN** CFG build, CFG verify, or direct-call scanning runs on the supported path
- **THEN** control-flow behavior still follows the structured semantics contract
- **AND THEN** the poison mnemonic only affects external text fields, not correctness behavior
