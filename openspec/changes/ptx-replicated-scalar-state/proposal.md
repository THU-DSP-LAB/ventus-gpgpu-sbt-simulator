## Why

The current leader-lane scalar-state implementation fixed correctness issues, but its physical representation has caused a severe PTX and `ptxas` quality regression.

The main problems are not the existence of a scalar-state ABI or the `value_blob` call shape by themselves. The regression comes from eagerly materializing nearly all Ventus scalar state as real PTX live state:

- `%x<256>` is kept as a long-lived physical PTX register file
- `vbranch/join` boundaries perform full-`x` broadcast via `shfl.sync`
- direct-call boundaries serialize and restore full mutable state eagerly

As a result, many Ventus `xreg` values that should remain cold or be optimized away become explicit PTX dataflow uses, pushing `ptxas` to extreme register pressure, spills, and compile-time growth.

Recent investigation also clarified an important boundary:

- `lab/06_ptx_call_boundary_dead_state` shows that `ptxas` can eliminate ABI-only dead state effectively
- therefore the more urgent issue is eager materialization and control-flow protocol design, not `value_blob` shape alone

This change redesigns scalar-state simulation so that the common case keeps active-lane scalar state replicated and equal, while leader-only execution is used only where required for correctness.

## What Changes

- Replace the current "leader lane owns canonical scalar PTX registers" model with an "active-lane replicated scalar state" model for Ventus scalar state that is live across PTX execution.
- Treat most uniform, side-effect-free scalar instructions as all-lane execution so active lanes compute the same value directly without leader broadcast.
- Restrict leader-only execution to instructions that are externally side-effecting or lane-sensitive, such as stores, atomics, traps, and scalarization-style instructions like `vmv.x.s`.
- Remove full-`x` broadcast as the default `vbranch/join` control-flow protocol.
- Keep direct-call support and full Ventus machine-state transfer semantics, but avoid eager full-state materialization at PTX call boundaries.
- Preserve the current correctness assumptions from Ventus divergence analysis: scalar state that remains live across `join` must stay warp-uniform on active lanes.

## Capabilities

### New Capabilities

- Simulate Ventus scalar state as replicated active-lane state instead of leader-owned PTX scalar state in the common case.
- Support path-local leader selection only when a leader-only instruction is actually needed inside a divergent region.
- Distinguish scalar instructions by execution semantics:
  - uniform-pure instructions run on all active lanes
  - lane-sensitive instructions run leader-only with explicit synchronization only when needed
  - externally side-effecting instructions run leader-only

### Modified Capabilities

- Direct-call lowering continues to pass Ventus machine state across PTX `.func` boundaries, but no longer treats eager full-state materialization as the default strategy.
- `vbranch/join` lowering no longer relies on full-`x` shuffle synchronization to maintain scalar-state correctness.
- Scalar loads are treated as all-lane operations under the current project assumptions:
  - no MMIO-like accesses
  - no special PTX memory proxies outside ordinary loads
  - no reliance on unsynchronized same-address read/write observation behavior

## Impact

- PTX text size, `shfl.sync` count, register pressure, and `ptxas` compile time are expected to drop substantially for kernels that currently suffer from full-`x` broadcast and eager scalar-state materialization.
- This change preserves current functional goals, but it depends on the existing Ventus compiler assumption that scalar state live across divergence remains uniform on active lanes.
- The implementation must classify scalar instructions carefully; misclassifying lane-sensitive instructions as all-lane would cause functional errors.
