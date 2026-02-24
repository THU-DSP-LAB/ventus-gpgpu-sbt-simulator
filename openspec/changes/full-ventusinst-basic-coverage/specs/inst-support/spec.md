# inst-support (delta spec)

## ADDED Requirements

### Requirement: Full `VentusInst_basic.txt` mnemonic coverage (compile-supported)
The project MUST support every instruction mnemonic listed in `VentusInst_basic.txt` through:
decode + CFG verify + PTX emit + `ptxas` compilation.

The only allowed exceptions are those explicitly documented under an “Exceptions” section, and they MUST be approved before being added.

#### Scenario: Compile-supported coverage gate
- **WHEN** the instruction support test suite is executed
- **THEN** it reports that all `VentusInst_basic.txt` mnemonics are compile-supported except documented exceptions

### Requirement: Full micro-test coverage for `VentusInst_basic.txt`
The project MUST provide micro-tests such that every mnemonic in `VentusInst_basic.txt` is exercised by at least one micro-test kernel.

Micro-tests MUST use the OpenCL buffer A/B pattern:
- inputs are provided via buffer A (or multiple input buffers),
- outputs are written into buffer B (or multiple output buffers),
- the host reads back buffers and compares Spike vs PTX results.

#### Scenario: Micro-test suite covers all mnemonics
- **WHEN** the micro-test coverage tool runs
- **THEN** it reports that all `VentusInst_basic.txt` mnemonics are covered except documented exceptions

### Requirement: Spike oracle comparison rules
For integer and bitwise operations, Spike vs PTX comparisons MUST match exactly.

For floating-point operations, comparisons MUST allow approximate equality (tolerance-based), and MUST handle NaN/Inf consistently.

#### Scenario: Float result is numerically close
- **WHEN** a float micro-test is executed on Spike and PTX backends
- **THEN** the outputs pass the configured tolerance comparison

### Requirement: Exceptions are explicit and stable
The project MUST maintain an explicit exception list for instruction support, and MUST keep it small.

The exception list MUST include at least:
- `jalr` non-`ret` forms (indirect jump/call)

Any additional exception MUST be added intentionally and MUST be visible in the spec and test reporting.

#### Scenario: New exception requires explicit documentation
- **WHEN** an instruction cannot be supported without breaking core invariants
- **THEN** it is added to the exception list with a rationale before being treated as allowed fail-fast

## REMOVED Requirements
None.

