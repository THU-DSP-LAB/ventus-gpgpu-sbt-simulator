## Context

> Status: `active target design`
>
> 本文描述 `model-builtin-call-semantics-in-cfg-verify` change 的目标设计，不表示当前仓库已经实现该行为。当前实现真相仍以 `README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`openspec/specs/` 和源码为准。

这个 change 是从旧代码分支迁移而来，但已按当前仓库结构重写。

当前相关实现事实：

1. `sbt/ptx_emit_builtin.cpp` 维护当前 builtin inline lookup table 和 builtin emission bodies。
2. `sbt/ptx_emit_control.cpp` 使用 builtin lookup 决定 direct call 是 inline builtin 还是普通 `.func` call。
3. `tools/sbt_ptx.cpp` 已经从 ELF `.symtab` 构建 `sym_by_addr`，但 `verify_function(cfg, func_name)` 没有接收它。
4. `tools/sbt_decode.cpp cfgverify` 也调用无 call-target context 的 verifier。
5. `sbt/cfg_verify.cpp` 的 vector-uniform must analysis 只应用普通 vector-dst instruction metadata；direct `jal rd!=0` 当前不更新任何 vector register fact。

因此 verifier 与 emitter 对 builtin helper call 的语义不一致。`_Z12get_local_idj` 在 emitter 中会把 `%tid.{x,y,z}` 写入 `%v0`，但 verifier 可能继续认为 `%v0` 保留调用前 `vmv.v.x` 建立的 uniform fact。

Canonical dependencies:

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/specs/sbt-rodinia-bringup/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`
- `sbt/cfg_verify.hpp`
- `sbt/cfg_verify.cpp`
- `sbt/ptx_emit_builtin.cpp`
- `sbt/ptx_emit_control.cpp`
- `sbt/ptx_emit_internal.hpp`
- `tools/sbt_ptx.cpp`
- `tools/sbt_decode.cpp`
- `tools/check_ptx_emit_name_allowlist.py`

## Goals / Non-Goals

**Goals:**

- Make builtin helper return semantics explicit and visible to CFG verification.
- Keep the emitter builtin inline lookup and verifier summaries synchronized through a shared metadata source and tests.
- Reject barrier-in-divergence cases such as current Rodinia `lud_diagonal` before PTX emission.
- Preserve useful uniform proofs for work-group-uniform builtin results, including `get_group_id(dim)` / `get_global_size(dim)` when the dim input is proven uniform.
- Treat direct calls without verifier-visible summaries as explicit conservative boundaries by clearing all vector-uniform facts.
- Keep diagnostics fail-fast and debuggable; do not add runtime timeout, mock success, fallback PTX generation, or silent degradation.

**Non-Goals:**

- Do not implement a software SIMT stack.
- Do not make divergent OpenCL barriers executable in PTX.
- Do not change `sbt::ptx::emit_module()` public behavior beyond consuming shared builtin metadata.
- Do not change direct-call `.func` ABI or call graph closure semantics.
- Do not add interprocedural vector-uniform inference for arbitrary non-builtin functions.
- Do not modify parent `ventus-env` subprojects.

## Decisions

### 1. Move builtin call identity and verifier summaries to shared metadata

The current builtin lookup lives in `sbt/ptx_emit_builtin.cpp`, while the verifier cannot include or depend on PTX internal emission state. Introduce a small shared module such as:

- `sbt/builtin_semantics.hpp`
- `sbt/builtin_semantics.cpp`

This module owns the builtin call identity currently represented by the `kBuiltinCalls` table. It should expose:

- `enum class BuiltinKind { ... }`
- `std::optional<BuiltinKind> lookup_builtin_call(std::string_view name)`
- `bool is_inlined_builtin_call_name(std::string_view name)`
- `std::optional<BuiltinSummary> builtin_summary_for(BuiltinKind kind)`
- an iterable list of accepted builtin entries for drift tests.

`sbt/ptx_emit_builtin.cpp` should keep emission bodies and dispatch, but consume `BuiltinKind` and lookup from the shared module. `sbt/ptx_emit_control.cpp` should continue to resolve direct calls through the same lookup before deciding builtin inline vs `.func` call. The public `sbt::ptx::is_inlined_builtin_call_name()` may remain as a compatibility wrapper, but it must delegate to shared metadata.

The summary describes verifier-visible vector writes, not PTX text. A compact shape is enough:

```cpp
enum class BuiltinUniformTransfer {
  WorkGroupUniform,
  WorkItemVarying,
  SameAsInputs,
  SameAsCorrespondingInput,
};

struct VectorWriteSummary {
  int dst_vreg;
  BuiltinUniformTransfer transfer;
  std::span<const int> input_vregs;
};

struct BuiltinSummary {
  BuiltinKind kind;
  std::span<const VectorWriteSummary> vector_writes;
};
```

Exact names are implementation details. The contract is that builtin semantic effects are data shared by verifier and emitter, not duplicated string branches. Summary inputs MUST be read from the pre-call vector-uniform state, and all summary writes MUST be applied as one call effect so in-place helpers cannot accidentally depend on write order.

### 2. Model the current builtin surface exactly

The initial summary table must match the current 22 accepted symbols in `sbt/ptx_emit_builtin.cpp`, not a future desired surface.

| Builtin family | Symbols | Vector writes | Uniform transfer |
|---|---|---:|---|
| `get_local_id` | `_Z12get_local_idj` | `v0` | work-item-varying |
| `get_global_id` | `_Z13get_global_idj` | `v0` | work-item-varying |
| `get_group_id` | `_Z12get_group_idj` | `v0` | uniform iff dim input `v0` is uniform |
| `get_global_size` | `_Z15get_global_sizej` | `v0` | uniform iff dim input `v0` is uniform |
| workitem id builtins | `__builtin_riscv_workitem_id_{x,y,z}` | `v0` | work-item-varying |
| global id builtins | `__builtin_riscv_global_id_{x,y,z}` | `v0` | work-item-varying |
| workgroup id builtins | `__builtin_riscv_workgroup_id_{x,y,z}` | `v0` | work-group-uniform |
| scalar unary math | `_Z10__clc_sqrtf`, `_Z4sqrtf` | `v0` | uniform iff `v0` is uniform |
| scalar binary math | `_Z4fmaxff` | `v0` | uniform iff `v0` and `v1` are uniform |
| scalar ternary math | `_Z5mad24iii` | `v0` | uniform iff `v0`, `v1`, and `v2` are uniform |
| vec4 unary math | `_Z3cosDv4_f`, `_Z3sinDv4_f`, `_Z3tanDv4_f`, `_Z4sqrtDv4_f`, `_Z4fabsDv4_f` | `v0..v3` | each output uniform iff corresponding input register is uniform |

For dimension-taking ID helpers, this change intentionally remains static and conservative. `get_local_id(dim)` and `get_global_id(dim)` are work-item-varying for any dimension because verifier does not receive launch shape. `get_group_id(dim)` and `get_global_size(dim)` select a work-group-uniform component based on input `%v0`; their result is proven uniform only when that dim input is already proven uniform. This can reject cases where a runtime dimension has size 1, but it is sound.

### 3. Extend verifier API with explicit call-target context

Add a verifier options object:

```cpp
struct VerifyOptions {
  const std::unordered_map<uint32_t, std::string> *sym_by_addr = nullptr;
};

FunctionVerifyResult verify_function(
    const FunctionCfg &cfg,
    std::string func_name,
    const VerifyOptions &options);
```

Keep the current two-argument overload only as an explicit compatibility path. It should call the options-bearing API with no symbol map, and the no-symbol direct-call behavior must clear all vector-uniform facts.

Production tools should pass symbols:

- `tools/sbt_ptx.cpp`: pass `sym_by_addr` for entry verification and all callee verification inside `build_cfg_for`.
- `tools/sbt_decode.cpp cfgverify`: build `sym_by_addr` from `read_func_symbols()` and pass it to verifier.

The verifier must not read ELF files directly. Symbol lookup is a caller-provided analysis context.

### 4. Apply call summaries in vector-uniform transfer

Change the transfer path from `transfer_vreg_uniform(st, bi)` to a call-aware variant that can see `VerifyOptions`.

Direct-call transfer:

1. Classify instruction with `sbt::control::classify(bi)`.
2. If it is not a direct call, apply the existing vector-dst instruction metadata path.
3. For direct call, resolve target PC from `semantics.direct_target`.
4. Look up symbol name in `options.sym_by_addr`.
5. If symbol resolves to an inlined builtin with a summary, apply the summary's vector writes using a pre-call state snapshot:
   - `WorkItemVarying`: set destination non-uniform.
   - `WorkGroupUniform`: set destination uniform.
   - `SameAsInputs`: set destination uniform iff all listed input registers are currently uniform.
   - `SameAsCorrespondingInput`: for vec4 helpers, each `vi` follows its own previous uniform fact.
6. If symbol resolves to a name accepted by the builtin inline lookup but no summary exists, fail fast as a metadata drift / contract error.
7. If target is unresolved, the symbol map is missing, or the symbol resolves to a non-builtin / non-inline callee without a verifier-visible summary, clear all vector-uniform facts.

For the first implementation, the conservative direct-call boundary SHALL clear all vector-uniform facts. That is sound and explicit because the current direct-call `.func` ABI can pass back the whole mutable vector state. The important point is that known builtin calls use precise summaries, accepted builtins missing summaries fail as drift, and resolved non-builtin callees are described as "no summary" rather than as unknown targets.

Both block fixed-point transfer and the within-block replay used to prove a specific `vbranch` must use the same call-aware transfer helper. Otherwise `vuni.in` and per-branch `st` can disagree.

### 5. Preserve diagnostics but make them causally correct

The existing barrier diagnostic already identifies that a barrier lies inside an unproven vbranch region. This change does not need a new diagnostic format to be useful.

For current `lud_diagonal`, expected facts after implementation:

- `_Z12get_local_idj` writes non-uniform `%v0`.
- `vbranch@0x80000194` and the later `vbranch@0x8000023c` using `%v0` are not proven uniform.
- barriers at `0x80000230` / `0x8000029c` are rejected when they lie inside the corresponding unproven branch region.
- `sbt_ptx` exits before writing usable PTX.

If implementation adds optional debug detail such as "call target `_Z12get_local_idj` writes lane-varying `%v0`", it must be explicit and tested, but this is not required for the core fix.

### 6. Tests and checks must match the current repo layout

This repository uses `tools/*_test.cpp` executables registered in `CMakeLists.txt`, not a top-level `tests/` tree.

Add a focused test executable such as `tools/cfg_verify_builtin_call_semantics_test.cpp` that constructs small CFGs or decodes small instruction sequences and checks:

- negative: uniform `%v0`, direct call to `_Z12get_local_idj`, `vbranch` using `%v0`, barrier in branch region => verifier rejects barrier;
- positive: direct call to `_Z12get_group_idj` with uniform dim feeding a structurally valid branch does not fail solely because of the builtin call;
- negative: direct call to `_Z12get_group_idj` with non-uniform dim does not create a uniform proof;
- negative: resolved ordinary helper call without summary clears all vector-uniform facts before a later `vbranch`;
- summary drift: every shared builtin lookup entry has a verifier summary, and every summary entry maps to an accepted builtin;
- missing symbol map or unresolved target clears all vector-uniform facts.

Also extend `tools/check_ptx_emit_name_allowlist.py` or add a C++ contract test so that builtin lookup, public `is_inlined_builtin_call_name()`, control inline dispatch, and verifier summary cannot drift.

Real-input checks:

- `timeout 60s build/sbt_decode cfgverify ../rodinia/opencl/lud/object0.riscv --func lud_diagonal --require-known` must return failure with barrier diagnostics.
- `timeout 60s build/sbt_ptx ../rodinia/opencl/lud/object0.riscv --func lud_diagonal --require-known --out /tmp/lud_diagonal.ptx` must fail before usable PTX output.
- `timeout 60s build/sbt_ptx ../rodinia/opencl/bfs/object0.riscv --func BFS_1 --require-known --out /tmp/BFS_1.ptx` must still pass.

## Risks / Trade-offs

- Conservative `get_local_id(dim)` / `get_global_id(dim)` modeling can reject launch-specific uniform cases. This is acceptable because current verifier has no launch metadata.
- Proving `get_group_id(dim)` / `get_global_size(dim)` uniform only when dim `%v0` is already uniform can reject cases where dynamic lane-varying dim values happen to select equal runtime values. This is sound.
- Clearing all vector-uniform facts for resolved non-builtin callees without summaries and unresolved direct calls can over-reject helper-heavy kernels. This is sound and explicit; later changes can add non-builtin summaries or interprocedural analysis.
- Moving builtin identity out of `ptx_emit_builtin.cpp` touches emitter ownership and CMake wiring. Focused drift tests are required so this does not recreate a split source of truth.
- The summary model must stay concrete. A symbolic execution engine or runtime launch-shape model is out of scope.
