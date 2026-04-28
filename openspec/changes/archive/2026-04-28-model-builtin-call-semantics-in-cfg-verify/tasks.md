## Implementation Tasks

- [x] 1. Baseline audit current code
  - [x] Confirm the current accepted builtin symbols in `sbt/ptx_emit_builtin.cpp` and record their emitted vector writes.
  - [x] Confirm current verifier API remains `verify_function(cfg, func_name)` and direct calls do not update vector-uniform facts.
  - [x] Record the current `lud_diagonal` behavior: `sbt_decode cfgverify` and `sbt_ptx` incorrectly pass.

- [x] 2. Add shared builtin semantic metadata
  - [x] Create `sbt/builtin_semantics.hpp` and `sbt/builtin_semantics.cpp` or an equivalent shared module.
  - [x] Move `BuiltinKind` and builtin symbol lookup ownership out of PTX-internal-only code.
  - [x] Expose `lookup_builtin_call()`, public inlined-builtin classification, iterable builtin entries, and verifier-visible summaries.
  - [x] Add summaries for all current 22 inlined builtin symbols.
  - [x] Register any new `.cpp` file in `CMakeLists.txt`.

- [x] 3. Rewire PTX emitter to shared builtin metadata
  - [x] Update `sbt/ptx_emit_builtin.cpp` to keep emission bodies but consume shared `BuiltinKind` / lookup.
  - [x] Update `sbt/ptx_emit_control.cpp` so inline builtin dispatch and ordinary `.func` call lowering use the shared lookup.
  - [x] Keep `sbt::ptx::is_inlined_builtin_call_name()` as a delegating compatibility/public API if still needed.
  - [x] Update `tools/check_ptx_emit_name_allowlist.py` if file names or lookup invariants change.

- [x] 4. Extend verifier call context
  - [x] Add `VerifyOptions` with optional `sym_by_addr`.
  - [x] Add `verify_function(cfg, func_name, options)` and route the current overload through it.
  - [x] Update `tools/sbt_ptx.cpp` entry verification and callee `build_cfg_for` verification to pass `sym_by_addr`.
  - [x] Update `tools/sbt_decode.cpp cfgverify` to build/pass `sym_by_addr`.

- [x] 5. Implement call-aware vector-uniform transfer
  - [x] Replace local `transfer_vreg_uniform(st, bi)` call sites with an options-aware helper.
  - [x] Apply builtin summaries for resolved inlined builtin direct calls.
  - [x] Treat resolved non-builtin / non-inline callees without summaries, unresolved targets, and missing-symbol-map direct calls as explicit conservative boundaries that clear all vector-uniform facts.
  - [x] Treat emitter-accepted builtin names that lack verifier summaries as fail-fast metadata drift / contract errors.
  - [x] Ensure the fixed-point block transfer and within-block vbranch replay use the same transfer logic.
  - [x] Preserve existing fail-fast behavior for missing ordinary instruction metadata.

- [x] 6. Add focused verifier tests
  - [x] Add `tools/cfg_verify_builtin_call_semantics_test.cpp` or equivalent.
  - [x] Cover `get_local_id(0)` feeding a `vbranch` with a barrier in the branch region and expecting rejection.
  - [x] Cover `get_group_id(0)` or `get_global_size(0)` with proven-uniform dim preserving a uniform result.
  - [x] Cover `get_group_id(dim)` or `get_global_size(dim)` with non-uniform dim not producing a uniform proof.
  - [x] Cover pure math builtin transfer from uniform and non-uniform inputs.
  - [x] Cover resolved ordinary helper call without summary clearing all vector-uniform facts.
  - [x] Cover missing symbol map / unresolved target conservative handling.
  - [x] Register the test executable in `CMakeLists.txt`.

- [x] 7. Add drift protection
  - [x] Add a C++ contract test or extend `tools/check_ptx_emit_name_allowlist.py` to prove every accepted builtin has a verifier summary.
  - [x] Prove every summary entry maps to a currently accepted builtin.
  - [x] Prove public `is_inlined_builtin_call_name()` delegates to the shared lookup.
  - [x] Prove an accepted builtin missing a verifier summary fails as metadata drift rather than silently using the non-builtin conservative path.

- [x] 8. Run real-input regression checks
  - [x] Verify `timeout 60s build/sbt_decode cfgverify ../rodinia/opencl/lud/object0.riscv --func lud_diagonal --require-known` fails with barrier convergence diagnostics.
  - [x] Verify `timeout 60s build/sbt_ptx ../rodinia/opencl/lud/object0.riscv --func lud_diagonal --require-known --out /tmp/lud_diagonal.ptx` fails before usable PTX output.
  - [x] Verify `timeout 60s build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx` still passes.

- [x] 9. Run focused automated checks
  - [x] Build with CMake.
  - [x] Run the new verifier builtin call semantics test.
  - [x] Run `external_mnemonic_contract_test`.
  - [x] Run `instruction_metadata_contract_test`.
  - [x] Run `ptx_emit_call_prototype_test`.
  - [x] Run `ptx_emit_leader_lane_abi_test`.
  - [x] Run `python3 tools/check_ptx_emit_name_allowlist.py`.

- [x] 10. Documentation sync
  - [x] Update `doc/IMPLEMENTATION_CODEMAP.md` to describe current call-aware builtin summary behavior after implementation.
  - [x] Update `tools/README.md` for any new test/check entry.
  - [x] Check whether `README.md` needs a user-facing note for `lud_diagonal` fail-fast diagnostics.
  - [x] Check whether `doc/README.md` needs an index update.
  - [x] Confirm `openspec/specs/replicated-scalar-state/spec.md` does not need a delta unless scalar-state semantics changed.

- [x] 11. OpenSpec consistency
  - [x] Run `openspec validate model-builtin-call-semantics-in-cfg-verify --type change --strict --no-interactive`.
  - [x] Confirm current/active/historical/legacy labels remain consistent.
  - [x] Confirm no fallback, mock success, timeout workaround, or silent degradation was introduced.
  - [x] After implementation and validation, sync this change's delta to `openspec/specs/sbt-rodinia-bringup/spec.md` through the normal OpenSpec flow.
