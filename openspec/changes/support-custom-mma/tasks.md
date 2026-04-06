## Implementation Tasks

- [ ] Canonicalize the MMA-only scope of this change.
- [ ] Record the boundary between `support-custom-mma` and `support-custom-instructions` in the active OpenSpec artifacts.
- [ ] Treat `doc/CUSTOM_INSTRUCTION_INPUT.md` as input material until the MMA support matrix is captured in canonical change/spec docs.
- [ ] Treat `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` as the frozen shared prerequisite for PTX baseline, oracle path, and MMA first-batch scope before code implementation.
- [ ] Treat `doc/mma/LOWERING_ARCHITECTURE.md` as the active lowering-architecture contract for the MMA-specific path.
- [ ] If implementation discovers that the shared baseline is insufficient, update `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` first, then update this change artifact, and only then proceed with code implementation.

- [ ] Define the MMA support matrix.
- [ ] Enumerate the Ventus MMA combinations by shape/layout/input-output type.
- [ ] Record the canonical master matrix in `doc/mma/LOWERING_ARCHITECTURE.md`, with committed first-batch entries only.
- [ ] Maintain a separate frontend-exposed inventory for uncommitted combinations, marked only as `deferred` / `research` / `TODO`.
- [ ] For each committed combination, classify it as `native-mma-sync`, `native-wmma`, `composite-lowering`, or explicitly unsupported.
- [ ] Record the required PTX target and lowering path for each matrix entry.
- [ ] Treat only the first committed `row.col` batch as Phase 1 required support: direct-native `m16n8*` plus committed `split-n` composite `m16n16*`; other layout pairs and composite forms must stay outside the first commitment unless the shared baseline is revised first.

- [ ] Define register-window / fragment mapping rules.
- [ ] Freeze the `VGPR window -> logical tile -> PTX fragment tuple` layered model for the supported families.
- [ ] Freeze the MMA decode metadata contract as `DecodedInst.custom.family = CustomFamily::Mma` plus a dedicated `MmaInstInfo` object.
- [ ] Document how Ventus A/B/C/D register windows map to logical tiles for each supported combination.
- [ ] Document how each supported logical tile maps to PTX fragments through explicit `PtxMmaAbiKey` / `PtxMmaAbiDesc` descriptors.
- [ ] Document any lane-layout assumptions required by the supported mappings.
- [ ] For first-batch direct-native families, instantiate PTX operand tuples through the same explicit tuple-materialization helpers used by composite lowering; do not guess from VGPR window order alone.
- [ ] For first-batch split-`n` composite families, freeze the block partitioning to Spike-equivalent `col_offset={0,8}` semantics and record the required `C/D` repack / merge steps explicitly.

- [ ] Front-load the validation asset before PTX lowering bring-up.
- [ ] Add a minimal MMA microtest for at least one first-batch committed family that makes the MMA result observable through buffers or equivalent external state.
- [ ] Run that minimal microtest on the Spike-backed path first and use it to confirm the repository-local input layout / output observation contract before PTX lowering work starts.
- [ ] Reuse the same observable microtest family as the seed of the later Spike-vs-PTX oracle gate rather than creating a disconnected one-off bring-up path.

- [ ] Implement the supported MMA subset only.
- [ ] Add MMA-specific decode support for the combinations marked supported by the matrix, while keeping ownership of shared custom-decode framework changes explicit.
- [ ] Add dedicated MMA metadata and lowering-plan handling without changing the behavior of already supported non-MMA lowering paths.
- [ ] Add PTX lowering for the supported combinations.
- [ ] Keep unsupported combinations on explicit fail-fast diagnostics.
- [ ] Do not expand the first committed batch beyond the `row.col` direct-native `m16n8*` and committed `split-n` `m16n16*` boundary frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` without first updating that shared baseline.

- [ ] Validate the supported MMA subset.
- [ ] Add compile-first validation for each supported combination at its required PTX target.
- [ ] Keep `tools/custom_decode_test.cpp` as the non-MMA gate until MMA decode is actually implemented there, and add a dedicated MMA decode test rather than silently broadening the existing gate.
- [ ] Extend the front-loaded observable MMA microtests into focused semantic tests that compare the Spike-backed path and the PTX path for each supported combination.
- [ ] If a repository-local helper model is retained during bring-up, keep it as an explicit cross-check rather than the sole semantic contract.

- [ ] Documentation sync:
- [ ] Update `README.md` with the current MMA support subset and target requirements once implemented.
- [ ] Update `doc/README.md`, `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, `doc/mma/LOWERING_ARCHITECTURE.md`, and `doc/IMPLEMENTATION_CODEMAP.md` with the MMA matrix and lowering boundary.
- [ ] Sync the resulting current contract into the relevant `openspec/specs/` files, especially `openspec/specs/inst-support/spec.md`.
- [ ] Check `openspec/README.md` index wording if the active change split needs explicit clarification.

- [ ] Final consistency check:
- [ ] Verify current/active/historical/legacy labels remain coherent after the split.
- [ ] Verify the non-MMA and MMA active changes do not claim overlapping implementation ownership.
