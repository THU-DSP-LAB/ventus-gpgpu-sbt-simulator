# build-time-spike-pattern-subset

## Purpose
Define how the Spike instruction pattern subset (from Spike `encoding.h` + `data/spike_want.txt`) is generated at build time and embedded into `sbt_decode`/`sbt_ptx` so runtime behavior is hermetic and reproducible.

## Requirements

### Requirement: Build-time generate and embed Spike pattern subset
The build system SHALL generate a Spike instruction pattern subset header at build time from:
- the repository-managed want list `data/spike_want.txt`, and
- a specified Spike `encoding.h` file.

The generated header SHALL be compiled into `sbt_decode` and `sbt_ptx` such that their Ventus pattern set is fixed by the binary.

#### Scenario: Want or encoding changes require rebuild
- **WHEN** the developer edits `data/spike_want.txt` (without changing source code)
- **OR WHEN** the developer changes the referenced Spike `encoding.h`
- **THEN** a subsequent build regenerates the subset header and recompiles affected targets

### Requirement: No runtime dependency on want/encoding files for `sbt_decode`/`sbt_ptx`
`sbt_decode` and `sbt_ptx` MUST NOT read want files or `encoding.h` at runtime to decode Ventus patterns.

#### Scenario: Tools run from arbitrary working directory without external files
- **GIVEN** `sbt_decode`/`sbt_ptx` are already built
- **AND GIVEN** the runtime filesystem does not contain a readable `data/spike_want.txt` at the current working directory
- **AND GIVEN** the runtime filesystem does not contain a readable `../spike/riscv/encoding.h` relative to the current working directory
- **WHEN** the user runs `sbt_decode decode <elf> --require-known`
- **AND WHEN** the user runs `sbt_ptx <elf> --func <kernel> --require-known`
- **THEN** both commands succeed or fail only due to input contents / translation support, and not due to missing want/encoding files

### Requirement: Remove runtime override entrypoints for pattern selection
The system SHALL remove runtime override entrypoints that can affect the pattern subset used by `sbt_decode`/`sbt_ptx`, including at least:
- `GPU_SBT_WANT_FILE`
- `--encoding-h` for `sbt_decode`
- `--encoding-h` for `sbt_ptx`

#### Scenario: CLI no longer exposes encoding override
- **WHEN** the user runs `sbt_decode --help` or provides invalid arguments
- **THEN** the usage output does not mention `--encoding-h`
- **AND WHEN** the user runs `sbt_ptx --help` or provides invalid arguments
- **THEN** the usage output does not mention `--encoding-h`

### Requirement: Build fails fast when inputs are unavailable or inconsistent
If the build-time `encoding.h` path is missing/unreadable, the build SHALL fail with a clear error message.

If any want id from `data/spike_want.txt` is not found as a `DECLARE_INSN` in the specified Spike `encoding.h`, subset generation SHALL fail with a clear error message naming the missing id.

#### Scenario: Missing encoding file causes build failure
- **GIVEN** the configured Spike `encoding.h` path does not exist
- **WHEN** the user builds the project
- **THEN** the build fails with a clear error identifying the missing `encoding.h`

#### Scenario: Want references an unknown instruction id
- **GIVEN** `data/spike_want.txt` contains an id not present in `encoding.h` `DECLARE_INSN(...)`
- **WHEN** the subset header is generated at build time
- **THEN** generation fails with a clear error naming the missing id

