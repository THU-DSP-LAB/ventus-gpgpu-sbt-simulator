## Implementation Notes

Status: `active implementation notes`

These notes record the as-found implementation layout before moving behavior out of
`sbt/ptx_emit_internal.hpp`. They are change-local working notes, not current
long-term project documentation.

## Baseline Header Inventory

`sbt/ptx_emit_internal.hpp` started this change at 1925 lines and mixed the
shared `EmitCtx` interface with substantial implementation bodies.

Implementation blocks by ownership:

- core/function assembly: fixed and virtual temp register declarations,
  entry/function header assembly, control protocol preparation, comments,
  shared precondition validation, `emit_one_inst`, CFG block traversal,
  fallthrough edge emission, and final `emit_body` assembly.
- runtime/PDS: lane/activemask helpers, leader predicate refresh, warp sync,
  inactive-lane trap, CSR/PDS computation, entry PDS pool acquire/release,
  entry prologue, and helper prologue.
- memory/address mapping: X-register load/store helpers, scalar wrappers,
  numeric shared/global address classification, typed load/store sequences,
  and leader-only memory wrappers.
- call ABI: helper function signature text, param load/store helpers,
  mutable-state blob store/restore, runtime/machine context blob marshal,
  kernel metadata scalar load helper, and direct `.func` call emission.
- builtin: OpenCL/RISC-V builtin emission bodies for id queries, global size,
  scalar float helpers, vector float4 helpers, and `mad24`.
- scalar FP: FP rounding normalization, PTX rounding suffix selection,
  `fclass.s`, and scalar FP lowering body.
- MMA materialization: MMA detail text, full-warp validation, shuffle helpers,
  tuple coordinate/index computation, A/B/C/D tuple materialization/merge,
  native/composite contract validation, native MMA emission, and top-level MMA
  instruction emission.

Shared `EmitCtx` interface that must remain visible to ownership units and
domain lowering files:

- fields binding CFG, symbol maps, function names, options, module info, entry
  status, output streams, label counter, FP note status, control protocol state,
  and virtual temp counters.
- fixed register naming helpers and ABI constants.
- scalar execution precondition helpers.
- basic body emission primitives (`emit_line`, `emit_raw`, `emit_label`).
- label, CFG block, and virtual temp allocation helpers.
- declarations for all moved member functions consumed across translation
  units.
