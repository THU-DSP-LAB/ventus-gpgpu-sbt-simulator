## Context

> Status: `active target design`
>
> 本文描述 `split-ptx-emit-internal-core` change 的目标设计，不表示当前仓库已经完成该结构收敛。当前实现真相仍以 `README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`openspec/specs/` 和源码为准。

当前 PTX emitter 已经具备下一步拆分的基础：

- `sbt/ptx_emit.cpp` 是薄 public/module assembly 入口；
- `sbt/ptx_emit_{control,scalar,vector,custom,mma_lowering}.cpp` 已经承载 domain entrypoints；
- current supported lowering authority 已经 descriptor/payload-driven；
- `tools/check_ptx_emit_name_allowlist.py` 已经覆盖 current emitter 文件集；
- `openspec/specs/ptx-lowering-modularity/spec.md` 已经把 shared core 与 domain lowering 分离列为 current contract。

但当前 `sbt/ptx_emit_internal.hpp` 仍有 1900+ 行，并且包含大量 out-of-interface implementation。它事实上成了新的单体实现位置。这个 change 的目标是把 internal header 收敛为 internal interface，将实现按 ownership 移入 `.cpp`，同时保持 public API 与 PTX behavior 不变。

Canonical dependencies:

- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/specs/ptx-lowering-modularity/spec.md`
- `openspec/specs/ptx-temp-register-allocation/spec.md`
- `openspec/specs/ptx-call-prototype/spec.md`
- `openspec/specs/replicated-scalar-state/spec.md`
- `openspec/specs/global-address-space/spec.md`
- `openspec/specs/inst-support/spec.md`
- `openspec/changes/split-ptx-emit-internal-core/specs/ptx-lowering-modularity/spec.md`

## Goals / Non-Goals

**Goals:**

- 将 `sbt/ptx_emit_internal.hpp` 缩成 internal interface，而不是大段 implementation 的承载点。
- 保留单一 `EmitCtx` shared host，但将其大成员函数 out-of-line 到职责明确的 `.cpp`。
- 让 runtime/PDS、memory/address mapping、call ABI、builtin、scalar FP、MMA materialization 都有清晰 ownership。
- 保持 domain dispatcher precedence 与 current lowering behavior 不变。
- 保持 builtin allowlist 与 builtin inline dispatch 单一事实源，避免 direct-call closure 与 lowering 分裂。
- 同步更新 CMake、allowlist 静态检查、regression 入口和 current/active 文档口径。

**Non-Goals:**

- 不重写 `EmitCtx` 为新的 facade/context 层。
- 不引入 handler registry、plugin、虚函数树或动态分派。
- 不改变 `sbt/ptx_emit.hpp` public API。
- 不改变 PTX 输出语义、ABI、fixed register ownership、address mapping 或 PDS 行为。
- 不新增指令支持或扩大 custom/MMA supported surface。
- 不将 `vector` domain 继续细拆为 vector-int/vector-fp/vector-cmp 等多个文件，除非实现过程中发现无额外耦合成本。
- 不修改上级 `ventus-env` 子项目源码。

## Decisions

### 1. Keep `EmitCtx` as the shared host, but move large method bodies out-of-line

保留 `EmitCtx` 是当前最低风险路径。domain lowering 文件已经围绕 `EmitCtx &ctx` 组织，现有 helper、temp allocation、label allocation、body emission、fixed registers、blob ABI 都以它为宿主。

设计目标不是消灭 `EmitCtx`，而是把 `EmitCtx` 从“包含所有实现的巨大 struct”调整为“声明共享 contract 的 internal host”：

- header 保留字段、构造函数声明、small inline primitive、helper/member declarations；
- 大块方法定义迁移到 ownership `.cpp`；
- domain lowering 继续调用 `ctx.emit_*` / `ctx.tmp_*` / `ctx.require_*` 等 internal API；
- 不让每个 domain 自己维护 body stream、temp counter 或 ABI state。

第一阶段可以接受 `EmitCtx` 仍是较重的 internal host，因为这个 change 的收益点在 implementation ownership，而不是抽象层重写。

**Alternatives considered:**

- 新建 `LoweringContext` facade 并隐藏 `EmitCtx`：Rejected。它会把一次 mechanical/ownership split 扩大成调用面重写，风险高于收益。
- 把所有 helper 改成 free functions 并传递多个 state object：Rejected。当前 state cohesion 在 `EmitCtx` 内是有意义的，拆散 state 会增加参数噪声。

### 2. Use ownership `.cpp` units instead of more public headers

新增实现文件以 `.cpp` 为主，避免创造多个新的 public-ish internal headers。建议初始文件边界：

- `sbt/ptx_emit_core.cpp`
  - `EmitCtx` constructor
  - fixed/virtual temp register declarations
  - entry/function body assembly orchestration
  - dispatcher: comment, shared preconditions, `emit_one_inst`
  - CFG block traversal, fallthrough emission, `emit_body`
  - generic labels / block target helpers where not kept inline
- `sbt/ptx_emit_runtime.cpp`
  - entry prologue and helper prologue
  - PDS pool acquire/release
  - CSR/PDS computation helpers
  - runtime/machine context restore helpers consumed by helper prologue
- `sbt/ptx_emit_memory.cpp`
  - numeric address mapping
  - typed load/store helpers
  - leader-only scalar memory wrappers
- `sbt/ptx_emit_call.cpp`
  - helper prototype/definition signature text and call parameter layout
  - mutable-state blob store/restore for helper entry/exit and direct call
  - outgoing machine/runtime blob marshal for direct call
  - direct `.func` call emission
- `sbt/ptx_emit_builtin.cpp`
  - public `is_inlined_builtin_call_name`
  - builtin lookup table / `BuiltinKind`
  - builtin inline dispatch and emission bodies
- `sbt/ptx_emit_scalar_fp.cpp`
  - FP rounding normalization
  - scalar `fclass`
  - scalar FP lowering body currently behind `ctx.try_emit_scalar_fp(di)`
- `sbt/ptx_emit_mma_lowering.cpp`
  - move real MMA materialization here from internal header
  - keep using `sbt/ptx_mma.*` for planner/ABI data

Existing domain files remain:

- `sbt/ptx_emit_control.cpp`
- `sbt/ptx_emit_scalar.cpp`
- `sbt/ptx_emit_vector.cpp`
- `sbt/ptx_emit_custom.cpp`

This is intentionally not a line-count balancing scheme. The boundary follows implementation ownership and regression risk.

The ownership map is an implementation-placement map, not a split of semantic authority. Shared contracts such as fixed registers,
virtual temps, blob layout, address-window rules, leader selection, and dispatcher preconditions still have one contract surface on
`EmitCtx` / shared primitives. Ownership `.cpp` files may define the method bodies, but they must not become independent authorities
for those contracts. In particular:

- core owns when function headers/body text are assembled, while call owns the helper ABI signature and call parameter layout text;
- runtime owns prologue-time restore/acquire/release behavior, while call owns outgoing direct-call marshal and helper exit mutable-state store;
- memory owns the single numeric address-mapping implementation consumed by scalar/vector/custom paths.

**Alternatives considered:**

- Create `ptx_emit_internal_memory.hpp`, `ptx_emit_internal_call.hpp`, etc.: Rejected for first phase. It risks turning one internal header into several semi-public headers before there is a proven need.
- Move only MMA out and leave the rest: Rejected. It fixes the most visible mismatch but leaves builtin/call/runtime/memory risks in the same internal monolith.

### 3. Make builtin allowlist and dispatch share one source of truth

Current public API includes `is_inlined_builtin_call_name(std::string_view)`, and the CLI call-graph closure uses it to avoid requiring `.func` emission for builtin calls. Control lowering then has a separate if/else dispatch sequence.

After the split, builtin handling should use one lookup source:

```cpp
enum class BuiltinKind { ... };
std::optional<BuiltinKind> lookup_builtin_call(std::string_view name);
bool is_inlined_builtin_call_name(std::string_view name) {
  return lookup_builtin_call(name).has_value();
}
void emit_builtin_call(EmitCtx &ctx, BuiltinKind kind, uint32_t pc);
```

`ptx_emit_control.cpp` should call `lookup_builtin_call(callee)` and then `emit_builtin_call(ctx, *kind, pc)`. The public `is_inlined_builtin_call_name()` delegates to the same lookup. This removes the current risk where allowlist accepts a name but dispatch lacks a matching implementation, or vice versa.

Builtin ownership remains in the call/control area. Some builtin bodies emit math-like PTX, but the semantic decision happens at direct-call resolution time.

The migration must add a structural check or focused unit assertion that enumerates the accepted builtin symbols and proves each accepted
symbol maps to a dispatchable `BuiltinKind`. This is stronger than testing a few representative names and protects the direct-call closure
from drifting away from inline emission.

**Alternatives considered:**

- Keep public allowlist and control dispatch as two independent if/else chains: Rejected because it preserves the exact sync risk this split should reduce.
- Move builtins into scalar/vector files based on emitted PTX shape: Rejected because it splits call resolution ownership and makes call ABI assumptions harder to audit.

### 4. Keep shared primitives centralized; do not clone contracts into ownership units

Moving implementation out of the header must not produce local contract clones. These remain single shared contracts:

- fixed `%r/%rd/%p/%x/%v` ownership
- `%tmp*` virtual temp allocation
- mutable/machine/runtime blob layout
- shared/global address-window rules
- leader selection and warp sync helpers
- dispatcher shared preconditions

Ownership `.cpp` files may contain private helper functions, but those helpers must compose shared primitives rather than redefining them. For example, scalar and vector memory lowering both continue to call the same address mapping helpers; they must not each create a local `[shared_base, global_base)` classifier.

**Alternatives considered:**

- Permit small local copies to reduce cross-file calls: Rejected. The code would look split but the real ABI/address contract would drift across files.

### 5. Split in safe migration slices, not by one large move

Implementation should proceed in behavior-preserving slices, each ending in a build or focused regression:

1. Create new `.cpp` files and add them to `CMakeLists.txt` with no behavior change.
2. Move core/body assembly methods out of the header.
3. Move memory/address mapping helpers.
4. Move call ABI and builtin lookup/dispatch, replacing duplicate builtin name checks with one lookup table.
5. Move scalar FP lowering.
6. Move MMA materialization into `ptx_emit_mma_lowering.cpp`.
7. Move runtime/PDS prologue/release helpers.
8. Trim internal header includes and ensure declarations remain sufficient.
9. Update static checks and docs.

The order deliberately leaves high-risk runtime/PDS and MMA work after the out-of-line pattern is proven. If a slice fails, it should be small enough to inspect by diff rather than requiring semantic debugging across the whole emitter.

**Alternatives considered:**

- Move every method in one patch: Rejected. Too much opportunity for accidental ordering, include, or linkage regressions.
- Start with runtime/PDS because it is large: Rejected. It is high-risk and should be moved after the new file pattern is validated.

### 6. Validation must check both behavior and structure

This change is mostly refactor, so semantic validation must prove “nothing changed,” and structural validation must prove “the intended boundary now exists.”

Required checks:

- build succeeds after CMake source updates and after each behavior-moving slice;
- `python3 tools/check_ptx_emit_name_allowlist.py` scans the full post-split file set;
- existing structural tests still pass:
  - `external_mnemonic_contract_test`
  - `instruction_metadata_contract_test`
  - `ptx_emit_call_prototype_test`
  - `ptx_emit_leader_lane_abi_test`
  - `custom_ptx_emit_test`
  - `mma_ptx_emit_test`
- at least one compile-first path through `sbt_ptx` + `ptxas`;
- if available, `tools/regress.sh --preset quick --arch sm_89`.

Structural post-checks should include:

- `ptx_emit_internal.hpp` no longer contains large bodies for runtime/PDS, memory, builtin, scalar FP, or MMA materialization;
- builtin lookup and public allowlist share one source;
- every accepted builtin symbol maps to an emitter path through the shared lookup / dispatch table;
- new files are covered by name allowlist scanning;
- docs list the new current file layout after implementation.

**Alternatives considered:**

- Rely only on compile-first: Rejected. Compile-first can miss allowlist/authority regressions and builtin closure inconsistencies.
- Rely only on structural diff: Rejected. Moving code across files can still break ABI/runtime behavior.

## Risks / Trade-offs

- **Risk: Behavior-preserving refactor accidentally changes PTX text ordering.** Mitigation: move methods out-of-line without changing body strings first; accept only formatting differences already covered by tests.
- **Risk: Builtin lookup table changes call graph behavior.** Mitigation: make public allowlist and control dispatch share the same `lookup_builtin_call()`; add/keep a test or assertion that every accepted builtin has an emitter path.
- **Risk: PDS acquire/release movement changes exit semantics.** Mitigation: move PDS code late and mechanically; cover with existing leader-lane/PDS smoke or quick regression where available.
- **Risk: Header trimming breaks domain files through missing includes.** Mitigation: use compile errors to add includes locally to the `.cpp` that needs them, not by re-expanding the internal header.
- **Risk: More `.cpp` files increase navigation overhead.** Mitigation: file boundaries mirror high-risk ownership domains; `doc/IMPLEMENTATION_CODEMAP.md` and `openspec/README.md` should document the layout.
- **Risk: `EmitCtx` remains large.** Accepted trade-off. This change targets implementation placement and ownership. A later facade split can be considered only after this boundary is stable.
- **Risk: Actual payoff is only cosmetic.** Mitigation: success criteria include reduced internal-header implementation ownership, single-source builtin lookup, full validation coverage, and docs alignment; not just lower line count.

## Self-Review

I reviewed the design against the requested concerns:

- **疏漏:** The initial coarse idea “move things out of internal header” was too vague; this design now names concrete ownership units and migration slices. It also includes docs and validation, which are required by repo policy.
- **谬误/矛盾:** The design does not claim the previous modularization failed. It treats the previous archived change as current foundation and scopes this as a follow-up refinement. This stays consistent with `openspec/README.md`, where this change is now listed as `active` and `2026-04-25-modularize-ptx-emit-lowering` remains `historical`.
- **可行性:** The plan keeps `EmitCtx` and public API stable, so most changes can be mechanical out-of-line member definitions plus CMake/check script updates. The riskiest behavior paths, PDS and MMA, are moved after easier slices.
- **风险:** The main risks are ABI/address/PDS/MMA behavior drift and builtin allowlist divergence. The design explicitly calls out single-source builtin lookup and required regressions.
- **实际收益:** The benefit is not abstract line-count reduction. The expected gain is narrower review ownership for runtime/PDS, memory, call ABI, builtin, scalar FP, and MMA materialization, plus preventing `ptx_emit_internal.hpp` from becoming the next long-term monolith.
