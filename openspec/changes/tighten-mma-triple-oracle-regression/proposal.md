## Why

Current MMA semantic validation is split:

- supported non-`fp16 -> fp16` families use Spike-vs-PTX comparison only,
- supported `fp16 -> fp16` families additionally use a repository-managed CPU reference,
- and the default input set is a single deterministic sample batch.

This leaves the current landed MMA subset with uneven oracle strength and makes regressions easier to miss when Spike and sbtsim happen to drift together. The regression contract should be tightened so every supported MMA family is checked against the same CPU reference model, while still keeping explicit blocked-path coverage for unsupported families.

## What Changes

- tighten the current MMA semantic gate so every supported MMA family uses `sbtsim / Spike / CPU reference` three-way comparison,
- expand MMA gate inputs from one deterministic batch to random finite-value samples with at least one small case and one larger case per supported family,
- simplify the MMA gate stage model so the current semantic path no longer depends on a separate Spike-only precheck stage,
- sync current repo docs and OpenSpec contract text to the new validation rule.

## Capabilities

### New Capabilities

- MMA regression can validate all supported current families against a shared repository-managed CPU reference.
- MMA regression can exercise multiple random sample batches at different sizes within one gate run.

### Modified Capabilities

- The current MMA oracle contract changes from mixed two-way/three-way comparison to uniform three-way comparison for all supported families.
- The default MMA gate flow changes from `spike-precheck -> compile-first -> full` to a compile-first path plus a full three-way semantic path.

## Impact

- MMA semantic regressions that previously required manual CPU-reference cross-checks become part of the default gate.
- Gate runtime will increase modestly because each supported family runs multiple random sample batches.
- Documentation Impact:
  - `README.md`
  - `doc/IMPLEMENTATION_CODEMAP.md`
  - `tools/README.md`
  - `openspec/specs/inst-support/spec.md`
