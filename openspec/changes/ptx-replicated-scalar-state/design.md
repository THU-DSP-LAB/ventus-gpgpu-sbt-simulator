## Context

The current PTX lowering path models Ventus scalar state with a persistent leader-lane ownership scheme. In practice this means:

- `%x<256>` is treated as a physical PTX scalar register file
- `vbranch/join` boundaries use full-`x` shuffle broadcast
- direct-call boundaries serialize and restore full mutable state eagerly

This solved earlier correctness gaps, but it also turned a large amount of logically cold Ventus scalar state into explicit PTX dataflow. The result is severe PTX growth, register pressure, spills, and `ptxas` compile-time regression.

Two investigation results shape this design:

1. `doc/ventus-divergence-sgpr-analysis.md`
   - the current input domain assumes scalar state live across `join` is uniform
   - lane-varying values that must survive divergence should not remain scalar
2. `lab/06_ptx_call_boundary_dead_state`
   - `ptxas` can eliminate ABI-only dead state effectively
   - therefore the main problem is eager materialization, not the blob-shaped call ABI by itself

The design goal is to preserve current Ventus machine-state semantics while changing the PTX physical representation so scalar state stays replicated and cheap in the common case.

## Goals / Non-Goals

**Goals:**
- Keep live Ventus scalar state equal on all active lanes.
- Remove full-`x` broadcast as the default structured-control-flow protocol.
- Restrict leader-only execution to instructions that truly require it.
- Preserve direct-call machine-state semantics without treating all call-boundary state as eager PTX live state.
- Make all assumptions explicit instead of silently adding fallback behavior.

**Non-Goals:**
- Prove legality for arbitrary hand-written Ventus ELF outside the current compiler-generated input domain.
- Change Ventus call semantics, caller-save/callee-save rules, or machine-state meaning.
- Solve every future MMIO, volatile, or exotic PTX memory-path case in this first design.
- Redesign vector register semantics beyond what is needed to avoid scalar-state regressions.

## Decisions

### 1. Replicated active-lane scalar state becomes the common-case representation

The common-case representation of live Ventus scalar state will be "replicated and equal on all active lanes", not "owned by one permanent leader lane".

This changes the physical invariant:

- old invariant: one leader lane carries canonical scalar truth, others receive it through synchronization
- new invariant: any live scalar value is already equal on the currently active lanes unless a leader-only instruction is in progress

This better matches the compiler contract for live scalar values after divergence and prevents `join` from forcing whole-register-file synchronization.

**Alternatives considered:**
- Keep permanent leader ownership and optimize broadcasts locally: rejected because the control protocol itself is what forces full-state materialization.
- Move scalar state back to shared backing storage: rejected because it avoids register explosion at the cost of reviving the earlier shared-memory traffic problem.

### 2. Scalar instructions are classified by semantics, not by “side effect or not”

The implementation will classify scalar-side instructions into three execution classes:

- `uniform-pure`
- `lane-sensitive`
- `externally side-effecting`

This is stricter than the initial heuristic "no side effect means all-lane". That heuristic is insufficient because some instructions are functionally lane-sensitive even without external side effects, such as vector-to-scalar extraction.

The initial classification policy is:

- scalar ALU and ordinary scalar CSR reads that depend only on uniform scalar inputs: all-lane
- fixed-lane scalarization instructions such as `vmv.x.s`: fixed-lane-sensitive, preserving their architectural source-lane meaning explicitly
- scalar stores, atomics, traps, and equivalent externally visible operations: leader-only
- ordinary scalar loads: all-lane under the current project assumptions

This makes correctness hinge on a finite explicit classification table instead of a vague emitter policy.

**Alternatives considered:**
- Use “side-effect-free means all-lane” as the only rule: rejected because it misclassifies lane-sensitive and fixed-lane-sensitive instructions and risks functional bugs.
- Keep most scalar instructions leader-only and just reduce synchronization sites: rejected because it preserves too much unnecessary leader protocol overhead.

### 3. Leader selection becomes lazy and path-local

The design keeps the concept of a leader, but only for instructions that require one.

Rules:

- no permanent leader ownership is required for all scalar execution
- a divergent path selects a leader only when it first reaches a leader-only instruction
- the selected leader must be chosen from that path’s current active mask
- if a path never executes a leader-only instruction, it never needs a path-local leader handoff

This removes the need for path-entry full-state handoff while still preserving correctness for stores, atomics, scalarization, and similar operations.

Leader-only scalar writes require an explicit follow-up protocol:

- after a leader-only instruction updates replicated scalar state, the affected destinations enter a temporary dirty-single-lane state
- before any later all-lane scalar consumer, scalar branch, call-state marshal, reconvergence consumer, or return that assumes replicated scalar state, the implementation must re-replicate those destinations to the current active subset
- this is a required correctness protocol, not an optional optimization

Fixed-lane-sensitive instructions are separate from arbitrary-leader execution:

- instructions such as `vmv.x.s` cannot substitute “current path leader” for their architectural source lane
- the implementation must either preserve the fixed-lane source semantics explicitly or reject unsupported shapes with an explicit error
- silent substitution of an arbitrary active leader is forbidden

**Alternatives considered:**
- Re-select a leader unconditionally at every path entry: rejected because it reintroduces control protocol cost even when the path only executes all-lane scalar code.
- Eliminate leader selection completely: rejected because single-execution operations still require exactly one executing lane.

### 4. Structured reconvergence no longer treats the whole xreg file as payload

`vbranch/join` lowering will no longer model the entire `xreg` file as a control-flow payload that must be shuffled across every reconvergence edge.

Instead:

- live scalar correctness after reconvergence comes from the replicated-state invariant and compiler uniformity contract
- leader-only scalar writes require explicit result synchronization for their affected outputs before any later replicated-state observation
- dead path-local scalar remnants are allowed to disappear without protocol cost

This is the main mechanism that removes the current full-`x` shuffle explosion.

**Alternatives considered:**
- Keep join-edge scalar-state repair but prune the register subset: rejected for the first design because it still begins from the wrong abstraction, namely that join must carry scalar-state payload explicitly.
- Add fallback merge logic for divergent scalar states: rejected because the supported input domain already assumes post-join live scalar values are uniform, and hidden merge logic would mask upstream legality problems.

### 5. Direct-call ABI keeps machine-state semantics but avoids eager full-state materialization

The design keeps the requirement that Ventus machine state remains semantically preserved across PTX `.func` calls.

However, it does not treat eager full-state serialization into PTX-visible live state as the default implementation strategy. The call ABI may remain `value_blob`, because the `lab/06` result shows that blob-shaped ABI-only dead state is not the main problem.

The design requirement is:

- preserve all semantically observed machine state across calls
- do not make ABI-only dead state explicit PTX live state by default
- do not assume call boundaries must force full scalar-state activation

This leaves room for implementation choices such as more selective materialization or backing-store-oriented handling, while preserving the semantic requirement.

**Alternatives considered:**
- Reject `value_blob` entirely and return to pointer/backing-store ABI: rejected because the evidence now shows the blob shape itself is not the dominant regression source.
- Keep current eager blob pack/unpack behavior: rejected because it defeats the dead-state elimination capability that `ptxas` can already provide.

### 6. Ordinary scalar loads are all-lane in the current supported domain

For the current supported program domain, ordinary scalar loads are treated as all-lane operations.

This is acceptable only under explicit assumptions:

- no MMIO-like semantics
- no special PTX proxy/non-coherent load path
- no correctness dependence on unsynchronized same-address read/write observation behavior

This avoids unnecessary leader protocol for a very common operation, while keeping the domain restriction explicit.

**Alternatives considered:**
- Make all scalar loads leader-only to preserve a single-observation model: rejected for the first design because it would preserve unnecessary leader overhead in the dominant current workload.
- Treat load behavior as unspecified and decide per call site later: rejected because it would make implementation inconsistent and difficult to verify.

## Risks / Trade-offs

- The design depends on the current Ventus compiler contract that post-`join` live scalar state is uniform. If future inputs violate that contract, this lowering can become incorrect.
- Instruction classification is now a critical correctness surface. Misclassifying a lane-sensitive instruction as all-lane would produce silent functional bugs.
- Keeping `value_blob` semantically while changing materialization strategy may still require careful implementation work to avoid accidentally recreating the same regression through a different code path.
- Treating scalar loads as all-lane is correct only within the explicit current input-domain assumptions. That decision must remain documented and easy to revisit if the supported domain expands.
