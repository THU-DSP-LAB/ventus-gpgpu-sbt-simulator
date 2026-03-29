## Implementation Tasks

- [ ] Canonicalize the active contract for this change before code implementation:
- [ ] Record the non-MMA custom instruction scope, the canonical `v0` ordinary-register rule, the current `vm`/`m`-bit semantics, and the split from the separate `support-custom-mma` change in the active OpenSpec artifacts.
- [ ] Treat `doc/CUSTOM_INSTRUCTION_INPUT.md` as input material rather than canonical current contract until the synced specs/docs are updated.
- [ ] Treat `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` as the frozen shared prerequisite for PTX baseline, oracle path, packed semantics, and MMA first-subset boundary before code implementation.
- [ ] If implementation discovers that the shared baseline is insufficient, update `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` first, then update this change artifact, and only then proceed with code implementation.

- [ ] Phase 1 foundation: extend front-end architecture for custom instructions.
- [ ] Add a repository-local custom decode path for the new custom opcode families so support no longer depends on Spike `DECLARE_INSN(...)`.
- [ ] Extend `DecodedInst` / decode metadata to carry the custom sub-op and dtype fields needed by this non-MMA change, while keeping the structure extensible for the separate MMA change.
- [ ] Keep ownership boundaries explicit: this change owns the shared custom-decode framework and non-MMA decode, while MMA-specific opcode semantics/metadata remain owned by `support-custom-mma`.
- [ ] Add decode-focused tests that prove `--require-known` accepts the new non-MMA instructions without requiring Spike whitelist changes.

- [ ] Phase 1 foundation: raise the PTX baseline for custom instruction support.
- [ ] Update PTX emitter target/version defaults to the shared baseline frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` (`.version 7.8` / `sm_90`).
- [ ] Update compile-first and regression entrypoints so the project defaults validate against the shared `sm_90` baseline rather than `sm_75`.
- [ ] Update the integrated runtime default in `../driver/driver/ptx_device/ventus.cpp` so the normal PoCL/driver path no longer clamps the default target SM to `75` once the shared baseline is adopted.
- [ ] Update runtime-facing docs/examples that currently rely on `VENTUS_PTX_SM=75` or `sm_75` defaults so the integrated flow matches the shared baseline rather than requiring a hidden environment override.

- [ ] Phase 1 lowering: implement all non-MMA custom instruction families.
- [ ] Implement the packed `f16x2` / `bf16x2` lowering against the shared 32-bit-container contract from `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, rather than redefining `vtype/vsew/vlmul` semantics locally in this change.
- [ ] Lower `shuffle.idx/up/down/bfly` through PTX `shfl.sync.*`.
- [ ] Lower `vcvt` instructions through PTX scalar/packed conversion instructions.
- [ ] Lower packed `f16x2` / `bf16x2` add/mul/fma.
- [ ] Lower `fp32` SFU instructions, using explicit composed sequences for `tanh/gelu/silu`.
- [ ] Lower packed `f16x2` / `bf16x2` SFU instructions, using explicit unpack/compute/repack logic where PTX lacks a native instruction.

- [ ] Phase 1 validation: add compile-first and semantic coverage for every non-MMA family.
- [ ] Add microtests that make each non-MMA family observable through output buffers.
- [ ] Ensure floating-point validation uses tolerance and packed/integer validation uses exact comparison where appropriate.
- [ ] Implement the repository-managed reference-model oracle path frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` rather than relying on an implicit default.
- [ ] Run the relevant compile-first and microtest coverage gates on the shared `sm_90` baseline.

- [ ] Documentation sync:
- [ ] Update `README.md` with the shared `sm_90` baseline and the current custom-instruction support scope.
- [ ] Update `doc/README.md`, `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, and `doc/IMPLEMENTATION_CODEMAP.md` to describe the new decode path, the shared PTX baseline, the oracle path, and the non-MMA / MMA change boundary.
- [ ] Sync the resulting current contract into the relevant `openspec/specs/` files, including `openspec/specs/inst-support/spec.md` and any affected Spike-pattern contract docs.
- [ ] Check `openspec/README.md` index wording if the current/active contract boundaries need clarification.

- [ ] Final consistency check:
- [ ] Verify current/active/historical/legacy labels remain coherent after the change artifacts and synced docs are updated.
- [ ] Verify there is only one active entry per topic and no parallel active doc replaces the same canonical entry.
