## Implementation Tasks

### 1. Freeze the current contract for the supported `fp16 -> fp16` family set

- [x] Sync this change's delta spec so `inst-support` expands the current landed MMA subset to include `m16n8k16 row.col f16->f16` and `m16n16k16 row.col f16->f16`.
- [x] Keep every other `fp16 -> fp16` MMA combination on explicit fail-fast diagnostics under `--require-known`.
- [x] Treat archived `support-custom-mma` blocked notes as `historical` background only once the new current contract lands.

### 2. Land the Spike-vs-CPU-reference validation assets first

- [x] Keep a repository-managed CPU reference for supported `fp16 -> fp16` MMA families with documented finite-input generation and tolerance rules.
- [x] Land or update a randomized `m16n8k16 row.col f16->f16` Spike-vs-CPU-ref test that uses finite `fp16` inputs in a reasonable range rather than hand-picked constants only.
- [x] Document the comparison rule for that test: finite values use `<= 1 fp16 ULP` or an equivalent documented host-side tolerance, and `NaN` compares by classification.
- [x] Add `m16n16k16 row.col f16->f16` Spike-vs-CPU-ref coverage based on the committed split-`n` semantics before or together with PTX lowering bring-up.

### 3. Replace the current blocked fail path with an explicit supported lowering contract

- [x] Remove `unsupported.mma.fp16_fp16_contract_pending` for the two current supported families only.
- [x] Add or extend the `M16N8K16_F16_F16` native MMA ABI descriptor coverage required by the PTX emitter.
- [x] Implement direct `VGPR window -> PTX tuple` helpers for `m16n8k16 row.col f16->f16`, driven by explicit Spike-equivalent logical-coordinate formulas instead of guessed raw register-window order.
- [x] If existing blocked-path code conflicts with the validated contract, replace or rewrite it instead of preserving it as an unverified compatibility path.
- [x] Keep the implementation aligned with `doc/mma/LOWERING_ARCHITECTURE.md` rather than introducing a second ad hoc fp16-only contract.

### 4. Reuse the validated native building block for split-`n`

- [x] Implement `m16n16k16 row.col f16->f16` as `SplitNInto2xM16N8` with exactly two native `m16n8k16 row.col f16/f16/f16/f16` sub-operations.
- [x] Freeze and implement `col_offset={0,8}` slice ownership for `B/C/D`, with shared logical `A`.
- [x] Reuse the same native ABI descriptor and direct window-to-tuple helpers for both sub-operations.
- [x] Add explicit merge/writeback checks so the combined result reconstructs the logical `16 x 16` tile without overlap or silent truncation.

### 5. Add end-to-end verification on the current `sm_89` baseline

- [x] Add compile-first coverage for `m16n8k16 row.col f16->f16` and `m16n16k16 row.col f16->f16` on `.version 7.8` / `sm_89`.
- [x] Add end-to-end semantic tests that compare `PTX vs Spike vs CPU reference` for `m16n8k16 row.col f16->f16` with randomized finite inputs and the documented tolerance.
- [x] Extend the same semantic gate to `m16n16k16 row.col f16->f16`.
- [x] Ensure unsupported/deferred/non-`row.col` or otherwise non-current `fp16 -> fp16` families still fail explicitly and are not silently reinterpreted.

### 6. Sync repository documentation and status labeling

- [x] Update `README.md` to move the two supported `fp16 -> fp16` families from blocked background into the current landed MMA subset.
- [x] Update `doc/README.md`, `doc/IMPLEMENTATION_CODEMAP.md`, `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, and `doc/mma/LOWERING_ARCHITECTURE.md` so they distinguish `current supported`, `active architecture`, and `historical blocked note` correctly.
- [x] Update `tools/README.md`, `openspec/README.md`, and `openspec/specs/inst-support/spec.md` to keep indexes and current contract wording aligned.
- [x] Keep `lab/07_fp16_mma_ptx_probe/` explicitly labeled as `active experiment` / design evidence rather than current implementation truth.

### 7. Final consistency check

- [x] Verify current/active/historical/legacy labels remain coherent across `README.md`, `doc/`, `openspec/README.md`, and the synced current spec.
- [x] Verify no document continues to describe these two `fp16 -> fp16` families in future tense after they become current behavior.
- [x] Verify the remaining blocked/deferred/research MMA families still have explicit ownership and fail-fast boundaries after this change lands.
