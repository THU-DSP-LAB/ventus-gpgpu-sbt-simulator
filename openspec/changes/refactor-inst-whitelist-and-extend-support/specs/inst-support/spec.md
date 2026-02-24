# inst-support (delta spec)

## ADDED Requirements

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

## REMOVED Requirements
None.
