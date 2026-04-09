## Implementation Tasks

- [x] Canonicalize the MMA-only scope of this change.
- [x] Record the boundary between `support-custom-mma` and `support-custom-instructions` in the active OpenSpec artifacts.
- [x] Treat `doc/CUSTOM_INSTRUCTION_INPUT.md` as input material until the MMA support matrix is captured in canonical change/spec docs.
- [x] Treat `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` as the frozen shared prerequisite for PTX baseline, oracle path, and MMA first-batch scope before code implementation.
- [x] Treat `doc/mma/LOWERING_ARCHITECTURE.md` as the active lowering-architecture contract for the MMA-specific path.
- [x] If implementation discovers that the shared baseline is insufficient, update `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` first, then update this change artifact, and only then proceed with code implementation.

- [x] Define the MMA support matrix.
- [x] Enumerate the Ventus MMA combinations by shape/layout/input-output type.
- [x] Record the canonical master matrix in `doc/mma/LOWERING_ARCHITECTURE.md`, with committed first-batch entries only.
- [x] Maintain a separate frontend-exposed inventory for uncommitted combinations, marked only as `deferred` / `research` / `TODO`.
- [x] For each committed combination, classify it as `native-mma-sync`, `native-wmma`, `composite-lowering`, or explicitly unsupported.
- [x] Record the required PTX target and lowering path for each matrix entry.
- [x] Treat only the first committed `row.col` batch as Phase 1 required support: direct-native `m16n8*` plus committed `split-n` composite `m16n16*`; other layout pairs and composite forms must stay outside the first commitment unless the shared baseline is revised first.

- [x] Define register-window / fragment mapping rules.
- [x] Freeze the `VGPR window -> logical tile -> PTX fragment tuple` layered model for the supported families.
- [x] Freeze the MMA decode metadata contract as `DecodedInst.custom.family = CustomFamily::Mma` plus a dedicated `MmaInstInfo` object.
- [x] Document how Ventus A/B/C/D register windows map to logical tiles for each supported combination.
- [x] Document how each supported logical tile maps to PTX fragments through explicit `PtxMmaAbiKey` / `PtxMmaAbiDesc` descriptors.
- [x] Document any lane-layout assumptions required by the supported mappings.
- [x] For first-batch direct-native families, instantiate PTX operand tuples through the same explicit tuple-materialization helpers used by composite lowering; do not guess from VGPR window order alone.
- [x] For first-batch split-`n` composite families, freeze the block partitioning to Spike-equivalent `col_offset={0,8}` semantics and record the required `C/D` repack / merge steps explicitly.

- [x] Front-load the validation asset before PTX lowering bring-up.
- [x] Add a minimal MMA microtest for at least one first-batch committed family that makes the MMA result observable through buffers or equivalent external state.
- [x] Run that minimal microtest on the Spike-backed path first and use it to confirm the repository-local input layout / output observation contract before PTX lowering work starts.
- [x] Reuse the same observable microtest family as the seed of the later Spike-vs-PTX oracle gate rather than creating a disconnected one-off bring-up path.

- [x] Implement the supported MMA subset only.
- [x] Add MMA-specific decode support for the combinations marked supported by the matrix, while keeping ownership of shared custom-decode framework changes explicit.
- [x] Add dedicated MMA metadata and lowering-plan handling without changing the behavior of already supported non-MMA lowering paths.
- [x] Add PTX lowering for the supported combinations.
- [x] Keep unsupported combinations on explicit fail-fast diagnostics.
- [x] Do not expand the first committed batch beyond the `row.col` direct-native `m16n8*` and committed `split-n` `m16n16*` boundary frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` without first updating that shared baseline.

- [x] Validate the supported MMA subset.
- [x] Add compile-first validation for each supported combination at its required PTX target.
- [x] Keep `tools/custom_decode_test.cpp` as the non-MMA gate until MMA decode is actually implemented there, and add a dedicated MMA decode test rather than silently broadening the existing gate.
- [x] Extend the front-loaded observable MMA microtests into focused semantic tests that compare the Spike-backed path and the PTX path for each supported combination.
- [x] If a repository-local helper model is retained during bring-up, keep it as an explicit cross-check rather than the sole semantic contract.

Blocked checkpoint (2026-04-07):
- `fp16 -> fp16` MMA lowering path is temporarily forced to explicit fail-fast due to ABI/metadata mismatch between Ventus LLVM and Spike (`openspec/changes/support-custom-mma/2026-04-06-mma-abi-metadata-mismatch-temp-note.md`). This block does not globally pause all MMA paths.
- 2026-04-09 update: the earlier helper-carrier ambiguity has been resolved by switching the finite carrier helpers in `testcases/ocl_compare/custom_mma_kernels.cl` to branch-free controlled lookup tables; `python3 tools/custom_mma_oracle.py --stage full --sm 89 --spike-compat-nested-regext` now passes for the full landed MMA subset, and this checkpoint remains only for the explicit `fp16 -> fp16` blocked family.
- Current handoff snapshot for next assignee is tracked in `openspec/changes/support-custom-mma/2026-04-07-support-custom-mma-handoff-status-temp-note.md` and should be treated as the active execution entry note (not a current spec replacement).

- [x] Documentation sync:
- [x] Update `README.md` with the current MMA support subset and target requirements once implemented.
- [x] Update `doc/README.md`, `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, `doc/mma/LOWERING_ARCHITECTURE.md`, and `doc/IMPLEMENTATION_CODEMAP.md` with the MMA matrix and lowering boundary.
- [x] Sync the resulting current contract into the relevant `openspec/specs/` files, especially `openspec/specs/inst-support/spec.md`.
- [x] Check `openspec/README.md` index wording if the active change split needs explicit clarification.

- [x] Final consistency check:
- [x] Verify current/active/historical/legacy labels remain coherent after the split.
- [x] Verify the non-MMA and MMA active changes do not claim overlapping implementation ownership.
