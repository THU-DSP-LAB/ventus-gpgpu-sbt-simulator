## Implementation Tasks

- [x] Canonicalize the active contract for this change before code implementation:
- [x] Record the non-MMA custom instruction scope, the canonical `v0` ordinary-register rule, the current `vm`/`m`-bit semantics, and the split from the separate `support-custom-mma` change in the active OpenSpec artifacts.
- [x] Treat `doc/CUSTOM_INSTRUCTION_INPUT.md` as input material rather than canonical current contract until the synced specs/docs are updated.
- [x] Treat `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` as the frozen shared prerequisite for PTX baseline, oracle path, packed semantics, and MMA first-subset boundary before code implementation.
- [x] If implementation discovers that the shared baseline is insufficient, update `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` first, then update this change artifact, and only then proceed with code implementation.

- [x] Phase 1 foundation: extend front-end architecture for custom instructions.
- [x] Add a repository-local custom decode path for the new custom opcode families so support no longer depends on Spike `DECLARE_INSN(...)`.
- [x] Extend `DecodedInst` / decode metadata to carry the custom sub-op and dtype fields needed by this non-MMA change, while keeping the structure extensible for the separate MMA change.
- [x] Keep ownership boundaries explicit: this change owns the shared custom-decode framework and non-MMA decode, while MMA-specific opcode semantics/metadata remain owned by `support-custom-mma`.
- [x] Add decode-focused tests that prove `--require-known` accepts the new non-MMA instructions without requiring Spike whitelist changes.

- [x] Phase 1 foundation: raise the PTX baseline for custom instruction support.
- [x] Update PTX emitter target/version defaults to the shared baseline frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` (`.version 7.8` / `sm_89`).
- [x] Update compile-first and regression entrypoints so the project defaults validate against the shared `sm_89` baseline rather than `sm_75`.
- [x] Update the integrated runtime default in `../driver/driver/ptx_device/ventus.cpp` so the normal PoCL/driver path no longer clamps the default target SM to `75` once the shared baseline is adopted.
- [x] Update runtime-facing docs/examples that currently rely on `VENTUS_PTX_SM=75` or `sm_75` defaults so the integrated flow matches the shared baseline rather than requiring a hidden environment override.

- [x] Phase 1 lowering: implement all non-MMA custom instruction families.
- [x] Implement the packed `f16x2` / `bf16x2` lowering against the shared 32-bit-container contract from `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, rather than redefining `vtype/vsew/vlmul` semantics locally in this change.
- [x] Lower `shuffle.idx/up/down/bfly` through PTX `shfl.sync.*`.
- [x] Lower `vcvt` instructions through PTX scalar/packed conversion instructions.
- [x] Lower packed `f16x2` add/mul/fma through native PTX packed arithmetic.
- [x] Lower `vfma.bf16x2` through native `fma.rn.bf16x2` on the shared `sm_89` baseline.
- [x] Lower `vadd.bf16x2` / `vmul.bf16x2` through explicit `bf16 -> f32 -> fp32 op -> bf16` composite sequences on the shared `sm_89` baseline.
- [x] Lower `fp32` SFU instructions, using explicit composed sequences for `tanh/gelu/silu`.
- [x] Lower packed `f16x2` SFU instructions, using explicit unpack/compute/repack logic where PTX lacks a native instruction.
- [x] Lower packed `bf16x2` SFU instructions through explicit `bf16 -> f32 -> fp32 SFU/composed sequence -> bf16` lowering on the shared `sm_89` baseline.

- [x] Phase 1 validation: add compile-first and semantic coverage for every non-MMA family.
- [x] Add microtests that make each non-MMA family observable through output buffers.
- [x] Ensure floating-point validation uses tolerance and packed/integer validation uses exact comparison where appropriate.
- [x] Implement the explicitly chosen oracle path frozen in `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, using Spike-backed OpenCL buffer comparison for the current non-MMA family set while keeping repository-managed reference models reserved for instruction families outside the current Spike-backed surface.
- [x] Run the relevant compile-first and microtest coverage gates on the shared `sm_89` baseline.

- [x] Documentation sync:
- [x] Update `README.md` with the shared `sm_89` baseline and the current custom-instruction support scope.
- [x] Update `doc/README.md`, `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md`, and `doc/IMPLEMENTATION_CODEMAP.md` to describe the new decode path, the shared PTX baseline, the oracle path, and the non-MMA / MMA change boundary.
- [x] Sync the resulting current contract into the relevant `openspec/specs/` files, including `openspec/specs/inst-support/spec.md` and any affected Spike-pattern contract docs.
- [x] Check `openspec/README.md` index wording if the current/active contract boundaries need clarification.

- [x] Final consistency check:
- [x] Verify current/active/historical/legacy labels remain coherent after the change artifacts and synced docs are updated.
- [x] Verify there is only one active entry per topic and no parallel active doc replaces the same canonical entry.
