## Implementation Tasks

- [ ] Refactor `sbt/ptx_emit.cpp` scalar-state helpers so live Ventus scalar state is modeled as replicated active-lane state instead of permanent leader-owned `%x` canonical state.

- [ ] Introduce an explicit scalar-instruction classification table and route lowering through it:
  - uniform-pure instructions execute all-lane
  - lane-sensitive instructions execute leader-only
  - fixed-lane-sensitive instructions preserve architecturally fixed source-lane semantics explicitly
  - externally side-effecting instructions execute leader-only
  - include at least `vmv.x.s`, scalar stores, atomics, traps, scalar CSR handling, and ordinary scalar loads in the first classification pass
  - add an explicit unsupported-input/error path for fixed-lane-sensitive cases that cannot be represented correctly under the current supported domain

- [ ] Replace the current `vbranch/join` scalar-state control protocol so structured divergence no longer depends on full-`x` broadcast:
  - remove unconditional path-entry and join-edge full-`x` shuffle behavior
  - add lazy path-local leader selection only when a leader-only instruction is actually encountered in a divergent path
  - add a required re-replication/dirty-state protocol after every leader-only scalar write before any later replicated-state consumer
  - preserve post-join live scalar-state correctness under the current compiler uniformity contract

- [ ] Redesign direct-call scalar-state materialization so PTX `.func` call boundaries preserve required Ventus machine-state semantics without default eager full-state scalar activation:
  - keep semantic preservation across call/ret
  - avoid turning ABI-only dead state into explicit long-lived PTX register state by default
  - ensure call-state marshalling never observes stale non-leader scalar copies after a leader-only scalar write
  - update helper ABI lowering paths consistently with the new scalar-state model

- [ ] Extend regression coverage for the new scalar-state model:
  - update or replace existing leader-lane emitter tests to reflect replicated scalar-state behavior
  - add focused tests for lane-sensitive vs all-lane classification
  - add focused tests for fixed-lane-sensitive instructions such as `vmv.x.s`
  - add tests for the required re-replication protocol after leader-only scalar writes
  - add tests covering lazy leader selection in divergent paths
  - add tests covering direct-call behavior under the new scalar-state representation

- [ ] Re-run relevant quality validation and capture evidence:
  - compile-first PTX smoke for affected kernels
  - targeted `ptxas -v` comparison on representative kernels that previously regressed
  - confirm `shfl.sync`, register pressure, spill, and compile-time trends improve relative to the current leader-lane full-broadcast implementation

- [ ] Synchronize documentation after code changes:
  - update `README.md`
  - update `doc/README.md`
  - update PTX-lowering design documents affected by the scalar-state model change
  - keep `lab/06_ptx_call_boundary_dead_state` referenced as evidence for the call-boundary dead-state conclusion
