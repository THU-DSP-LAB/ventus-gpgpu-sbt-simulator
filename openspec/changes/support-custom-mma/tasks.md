## Implementation Tasks

- [ ] Canonicalize the MMA-only scope of this change.
- [ ] Record the boundary between `support-custom-mma` and `support-custom-instructions` in the active OpenSpec artifacts.
- [ ] Treat `doc/CUSTOM_INSTRUCTION_INPUT.md` as input material until the MMA support matrix is captured in canonical change/spec docs.
- [ ] Treat `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` as the frozen shared prerequisite for PTX baseline, oracle path, and MMA first-subset scope before code implementation.
- [ ] If implementation discovers that the shared baseline is insufficient, update `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` first, then update this change artifact, and only then proceed with code implementation.

- [ ] Define the MMA support matrix.
- [ ] Enumerate the Ventus MMA combinations by shape/layout/input-output type.
- [ ] For each combination, classify it as `native-mma-sync`, `native-wmma`, `composite-lowering`, or explicitly unsupported.
- [ ] Record the required PTX target and lowering path for each matrix entry.
- [ ] Treat only the `row.col` combinations from the first committed `native-mma-sync` subset as Phase 1 required support; other layout pairs must stay outside the first commitment unless the shared baseline is revised first.

- [ ] Define register-window / fragment mapping rules.
- [ ] Document how Ventus A/B/C/D register windows map to PTX fragments for each supported combination.
- [ ] Document any lane-layout assumptions required by the supported mappings.

- [ ] Implement the supported MMA subset only.
- [ ] Add MMA-specific decode support for the combinations marked supported by the matrix, while keeping ownership of shared custom-decode framework changes explicit.
- [ ] Add PTX lowering for the supported combinations.
- [ ] Keep unsupported combinations on explicit fail-fast diagnostics.
- [ ] Do not expand the first committed subset beyond the `native-mma-sync row.col` boundary frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` without first updating that shared baseline.

- [ ] Validate the supported MMA subset.
- [ ] Add compile-first validation for each supported combination at its required PTX target.
- [ ] Add focused semantic tests that make each supported MMA result observable and comparable against the previously chosen oracle path.

- [ ] Documentation sync:
- [ ] Update `README.md` with the current MMA support subset and target requirements once implemented.
- [ ] Update `doc/README.md`, `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, and `doc/IMPLEMENTATION_CODEMAP.md` with the MMA matrix and lowering boundary.
- [ ] Sync the resulting current contract into the relevant `openspec/specs/` files, especially `openspec/specs/inst-support/spec.md`.
- [ ] Check `openspec/README.md` index wording if the active change split needs explicit clarification.

- [ ] Final consistency check:
- [ ] Verify current/active/historical/legacy labels remain coherent after the split.
- [ ] Verify the non-MMA and MMA active changes do not claim overlapping implementation ownership.
