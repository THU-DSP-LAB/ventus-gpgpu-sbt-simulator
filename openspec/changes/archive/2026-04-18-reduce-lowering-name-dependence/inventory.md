# Emitter Name-Dependence Inventory

状态：`active`

## Scope

本清单只覆盖本 change 的 **emitter correctness path**。

- `current`: `sbt/ptx_emit.cpp` 的 supported emit path 已改为消费 `DecodedInst.emit` / `DecodedInst.custom` / `DecodedInst.mma`
- `deferred`: `sbt/riscv_decode.cpp`、`sbt/instruction_metadata.cpp` 的内部 name-keyed metadata 组织方式，以及 `sbt/cfg.cpp` / `sbt/cfg_verify.cpp` 的 name 分类，留给后续 change

## Migrated Emitter Domains

以下语义域已从 emitter 内的 `di.name` 语义分派迁移为显式 descriptor / payload：

- control-flow
  - scalar branch / vector branch
  - direct jump / direct call / ret / indirect terminator
- structured control
  - `setrpc` / `join` / `barrier` / `vsetvli` / `endprg`
- scalar-side
  - scalar memory
  - scalar integer / bitmanip
  - scalar FP
  - CSR
  - scalar execution classification applicability
- vector ordinary
  - vector memory / PDS
  - vector register / merge / `vmv_x_s`
  - vector integer
  - vector compare / mask
  - vector convert / class
  - vector FP
- custom non-MMA
  - `shuffle`
  - `vcvt`
  - packed arithmetic
  - SFU
- MMA
  - 继续由 `MmaInstInfo` 驱动

## Remaining Emitter Name Allowlist

`sbt/ptx_emit.cpp` 中允许保留的 `name` 读取只剩：

- diagnostics
  - `EmitError(..., di.name)`
  - `invalid.scalar_exec` / `missing.scalar_exec_metadata`
- comments
  - `emit_line("// <pc> <mnemonic>")`

这些位置由 `tools/check_ptx_emit_name_allowlist.py` 静态检查。

## Deferred Non-Emitter Sites

以下 name-dependent site 已记录但 **不计入本 change 完成条件**：

- `sbt/instruction_metadata.cpp`
  - shared metadata 仍以 mnemonic name 查表
- `sbt/riscv_decode.cpp`
  - Spike-backed pattern decode 仍先产出 mnemonic name 再填 shared metadata
- `sbt/cfg.cpp`
  - basic block terminator / edge 分类仍使用 name
- `sbt/cfg_verify.cpp`
  - `setrpc` / `join` / `barrier` 等结构化验证仍使用 name
