## Context

This design describes the target behavior to be implemented by this change.

Current current-spec context:

- `openspec/specs/inst-support/spec.md`
- `README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `tools/README.md`

Today the MMA semantic gate is split:

- current supported non-`fp16 -> fp16` families only compare Spike and sbtsim PTX outputs,
- current supported `fp16 -> fp16` families additionally compare against a repository-managed CPU reference,
- and the gate feeds one deterministic input batch only.

The change makes the semantic oracle uniform across all current supported MMA families.

## Goals / Non-Goals

**Goals:**

- Use one repository-managed CPU reference path for every current supported MMA family.
- Keep blocked/deferred MMA families on explicit fail-fast / blocked diagnostics.
- Exercise at least one smaller and one larger warp-multiple random sample batch per supported family.
- Keep compile-first validation available while simplifying the semantic stage model.

**Non-Goals:**

- Expand the supported MMA family set.
- Introduce NaN/Inf-heavy stress testing in this change.
- Change sbtsim lowering behavior outside what is needed for the new oracle contract.

## Decisions

### 1. CPU reference becomes the canonical semantic oracle for all supported MMA families

The semantic gate will compare both Spike and sbtsim PTX outputs against one repository-managed CPU reference for every current supported family.

For `fp16 -> fp16`, the gate keeps the existing `<= 1 ULP` / NaN-classification comparison rule.

For `f32` accumulator families, the gate computes the same logical tile on CPU and compares both Spike and sbtsim PTX outputs to that CPU reference with documented `atol/rtol`.

**Alternatives considered:**

- Keep mixed validation (`Spike-vs-PTX` for `f32`, triple compare only for `fp16`): rejected because the oracle strength stays uneven and shared drift remains harder to catch.

### 2. Random finite samples use multiple batch sizes inside one gate run

The gate will replace one fixed deterministic batch with multiple reproducible random finite seed batches.

The implementation will run at least:

- one smaller warp-multiple batch,
- one larger warp-multiple batch.

Randomness is reproducible by deriving per-case RNG seeds from a CLI base seed. Inputs remain finite-value-only in this change to avoid conflating semantic regressions with NaN/Inf policy work.

**Alternatives considered:**

- Use non-deterministic host randomness: rejected because regressions become harder to reproduce.
- Keep one deterministic batch only: rejected because sample diversity is too weak.

### 3. Remove the dedicated Spike-only precheck stage from the main MMA gate contract

Once every supported family is checked by `compile-first` and full three-way semantic comparison, the separate `spike-precheck` stage no longer adds unique contract value.

The gate will keep:

- `compile-first`
- `full`

`full` becomes the default semantic path and includes both backend runs plus CPU-reference comparison.

**Alternatives considered:**

- Preserve `spike-precheck` for backward compatibility: rejected because it keeps an extra mixed-mode contract alive without distinct semantic coverage.

## Risks / Trade-offs

- Gate runtime increases because each family runs more than one sample batch and also computes CPU reference.
- The CPU reference must exactly mirror the kernel-side input materialization formulas; otherwise the gate can fail due to oracle drift instead of lowering bugs.
- Removing `spike-precheck` may require small documentation and script updates for any internal users that relied on that stage name.
