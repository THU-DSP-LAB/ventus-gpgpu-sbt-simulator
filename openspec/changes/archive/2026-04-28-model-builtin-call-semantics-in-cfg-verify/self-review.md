# Self Review

## Scope

This review covers the migrated current-repo artifacts:

- `proposal.md`
- `design.md`
- `tasks.md`
- `specs/sbt-rodinia-bringup/spec.md`

The source material was `../sbtsim-merge-ai/openspec/changes/model-builtin-call-semantics-in-cfg-verify`, but the migrated version is not a direct copy. It has been adjusted to current code in `/work/ventus-env-github/sbtsim`.

## Current-Code Adjustments

The migration updates these obsolete assumptions from the old change:

- Builtin inline ownership is now `sbt/ptx_emit_builtin.cpp`, not a monolithic `ptx_emit.cpp`.
- Current emitter already has `ptx_emit_{core,runtime,memory,call,builtin,control,scalar,vector,custom,mma_lowering,scalar_fp}.cpp` split.
- Current verifier still lacks `VerifyOptions` and does not receive `sym_by_addr`.
- Current verifier does not have a temporary "clear all vector-uniform facts for every direct call" behavior; it effectively ignores direct call vector return effects. The migrated design therefore frames the fix as adding call-aware transfer, with precise summaries for known builtins and an explicit clear-all conservative boundary for calls without summaries.
- Current tests live under `tools/*_test.cpp` and are registered in `CMakeLists.txt`; the migrated tasks do not assume a top-level `tests/` directory.
- Existing `tools/check_ptx_emit_name_allowlist.py` already checks current emitter name usage and builtin lookup/dispatch shape, so the new drift protection is described as an extension or C++ contract test, not as a brand-new concept.

## Necessity Check

The change is still necessary.

Current empirical behavior before implementation:

- `build/sbt_decode cfgverify ../rodinia/opencl/lud/object0.riscv --func lud_diagonal --require-known --verbose` reports `ok=true`, `vbranch_ok=8/8`, and `barrier_ok=4/4`.
- The same output marks `vbranch@0x80000194` as `proven_uniform=true`.
- `build/sbt_ptx ../rodinia/opencl/lud/object0.riscv --func lud_diagonal --require-known --out /tmp/lud_diagonal.ptx` succeeds.
- Disassembly shows `_Z12get_local_idj` is called after uniform initialization of `%v0`.
- Generated PTX shows the builtin inline writes `%tid.x` to `%v0`.

This proves the old bug class still exists in current code.

## Feasibility

The migrated design is feasible with localized ownership:

- `tools/sbt_ptx.cpp` and `tools/sbt_decode.cpp` already have access to ELF function symbols.
- `sbt/control_semantics.cpp` already classifies direct calls and computes direct targets.
- `sbt/ptx_emit_builtin.cpp` already centralizes current builtin symbol lookup.
- `sbt/cfg_verify.cpp` already has a vector-uniform transfer hook that can be extended to call-aware behavior.

The largest code movement is extracting builtin identity from PTX-internal code into shared metadata. This is compatible with the current emitter split and reduces drift risk.

## Risks

- Conservative clear-all handling of resolved non-builtin callees without summaries and unresolved direct calls can over-reject kernels with helper calls before `vbranch` / `barrier` regions. This is sound and explicit.
- Conservative static handling of `get_local_id(dim)` / `get_global_id(dim)` may reject launch-specific uniform cases. This is acceptable because verifier does not have launch shape.
- `get_group_id(dim)` / `get_global_size(dim)` must depend on the pre-call dim `%v0` uniform fact; treating them as unconditionally work-group-uniform would be unsound when dim is lane-varying.
- Moving builtin lookup ownership can break emitter dispatch if not done with drift tests.
- Focused tests must build valid CFG shapes; otherwise they can test malformed structure instead of builtin uniform transfer.

## Overall Assessment

The migrated plan is still the right fix for current code. The core requirement remains: verifier and emitter must share builtin helper call semantics before verifier proves `vbranch` uniformity and barrier legality. The updated artifacts remove stale file/layout assumptions and make implementation tasks match the current repository structure.
