## Implementation Tasks

- [x] Extend the repository-managed MMA CPU reference path so every current supported MMA family can be checked against CPU reference, not only `fp16 -> fp16`.
- [x] Update `tools/custom_mma_oracle.py` to use three-way `sbtsim / Spike / CPU reference` comparison for every supported family.
- [x] Replace the single deterministic MMA input batch with reproducible random finite samples that cover at least one smaller and one larger warp-multiple case per supported family.
- [x] Simplify the MMA gate stage model by removing the dedicated `spike-precheck` stage from the current path and keeping `compile-first` plus default `full`.
- [x] Keep unsupported/deferred MMA families on explicit blocked diagnostics and verify the blocked probe still fails for the expected reason.
- [x] Sync `README.md`, `doc/IMPLEMENTATION_CODEMAP.md`, `tools/README.md`, `openspec/README.md`, and `openspec/specs/inst-support/spec.md` to the new current oracle contract.
- [x] Final consistency check: verify the updated gate passes and current/active/historical labels remain coherent across repo docs and OpenSpec artifacts.
