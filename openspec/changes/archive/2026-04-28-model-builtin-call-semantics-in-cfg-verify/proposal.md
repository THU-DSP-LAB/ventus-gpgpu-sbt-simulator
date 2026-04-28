## Why

当前 CFG verifier 与 PTX emitter 仍没有共享 builtin helper call 的返回语义。

当前仓库中，PTX emitter 已拆分为 `sbt/ptx_emit_{core,runtime,memory,call,builtin,control,scalar,vector,custom,mma_lowering,scalar_fp}.cpp`。其中 `sbt/ptx_emit_builtin.cpp` 维护 22 个 builtin inline symbol，并在 direct-call lowering 时把 `_Z12get_local_idj`、`_Z12get_group_idj`、少量 math helper 等调用直接内联到 PTX。`tools/sbt_ptx.cpp` 已经构建 `sym_by_addr`，但 `sbt/cfg_verify.hpp` 仍只暴露 `verify_function(cfg, func_name)`；`sbt/cfg_verify.cpp` 的 vector-uniform must 分析只消费普通指令 metadata，看不到 direct `jal` callee 对 `%v0` / `%v0..%v3` 的返回写入。

这会让 verifier 保留调用前的 stale vector-uniform fact。当前实测 `../rodinia/opencl/lud/object0.riscv --func lud_diagonal`：

- `0x800000ec` 先执行 `vmv.v.x v0, zero`，使 `%v0` 看似 uniform；
- `0x800000f0` 调用 `_Z12get_local_idj`，PTX inline 实际把 `%tid.x` 写入 `%v0`；
- 后续 `vbranch@0x80000194` 使用 `%v0`，当前 `sbt_decode cfgverify` 仍报告 `proven_uniform=true`；
- 当前 `sbt_ptx` 会继续生成包含 `bar.sync` 的 PTX，而不是在 translation-time fail fast。

期望行为是：verifier 在证明 `vbranch` 收敛和 barrier 合法性之前，应用与 emitter inline 行为一致的 builtin call semantic summary。`get_local_id` / `get_global_id` 一类返回 work-item-varying 值的 builtin 必须打破 stale uniform proof；`get_group_id(dim)` / `get_global_size(dim)` 只有在 dim 输入 `%v0` 已 proven uniform 时才能保留有效证明；固定维度的 workgroup id builtin 可无条件保留 work-group-uniform 证明。没有 verifier-visible summary 的 direct call（包括 resolved non-builtin callee 与 unresolved target）必须清空全部 vector-uniform facts，而不是静默穿透旧事实。

## What Changes

- 新增当前仓库可共享的 builtin semantic metadata，使 verifier 和 emitter 使用同一个 builtin symbol / kind source。
- 将 CFG verifier API 扩展为可接收 call-target symbol map 的 options/config；`tools/sbt_ptx.cpp` 与 `tools/sbt_decode.cpp cfgverify` 必须把 `.symtab` 解析出的 `sym_by_addr` 传入 verifier。
- 在 vector-uniform transfer 中识别 direct call：
  - resolved inlined builtin：按 summary 更新 `%v0` / `%v0..%v3` uniform fact；
  - resolved non-builtin / non-inline callee without summary：清空全部 vector-uniform facts；
  - unresolved / no-symbol direct call：清空全部 vector-uniform facts；
  - emitter-accepted builtin missing summary：作为 metadata drift / contract error fail fast。
- 增加 focused verifier 测试，覆盖 `get_local_id(0)` 触发 barrier rejection、`get_group_id(0)` 在 uniform dim 下保留 proof、`get_group_id(dim)` 在 non-uniform dim 下不产生 proof、ordinary resolved helper call 清空 facts、builtin summary 与 emitter allowlist 防漂移、缺失符号图时的保守行为。
- 增加 real-input regression：`lud_diagonal` 应在 `sbt_decode cfgverify` / `sbt_ptx --require-known` 阶段 fail fast，`BFS_1` 等 smoke translation 继续通过。
- 同步 current 文档和 spec，明确 verifier 现在具有 call-aware builtin summary 行为。

本 change 不做：

- 不实现 software SIMT stack；
- 不让 divergent OpenCL barrier 在 PTX 中可执行；
- 不改变 CUDA driver launch path；
- 不改变 direct-call `.func` ABI；
- 不为 arbitrary non-builtin helper 增加 interprocedural summary；
- 不引入 timeout、mock success、runtime fallback 或 silent degradation。

## Capabilities

### New Capabilities

- CFG verifier 可以基于 builtin helper return semantics 更新 vector-uniform facts。
- Emitter builtin inline allowlist 与 verifier-visible summary 共享单一 metadata source，防止 drift。
- Barrier legality 检查可以在 translation-time 暴露 builtin-derived divergent branch，而不是把问题推迟到 CUDA `bar.sync` runtime hang。

### Modified Capabilities

- `sbt-rodinia-bringup` 的 barrier convergence verification 从 caller-only vector-uniform metadata 扩展为 call-aware vector-uniform analysis。
- direct call without verifier-visible summary 从“未建模但事实可能穿透”改为“显式清空全部 vector-uniform facts 的保守边界”。
- `tools/sbt_decode cfgverify` 与 `tools/sbt_ptx` 的 verifier 调用需要携带当前 ELF symbol map。

## Impact

- Rodinia `lud_diagonal` 当前应从误通过变为 deterministic fail-fast，并给出 barrier 位于不可证明收敛 vbranch 区域内的 diagnostic。
- 之前依赖 stale uniform facts 通过的 kernel 可能开始失败；这是正确性暴露，不是回归 workaround。
- 使用 `get_group_id`、`get_global_size` 或 pure math builtin 且 summary 所需输入已 proven uniform 的分支不应仅因 builtin call 出现而被拒绝。
- `BFS_1` 等无此 divergent barrier hazard 的 compile-first smoke 应保持通过。

## Documentation Impact

- `doc/IMPLEMENTATION_CODEMAP.md`：更新 current verifier 行为与 builtin semantic metadata ownership。
- `tools/README.md`：若新增 `cfg_verify_builtin_call_semantics_test` 或扩展静态检查，更新工具/测试索引。
- `README.md`：仅当 user-facing diagnostics 或常用命令说明变化时更新。
- `openspec/specs/sbt-rodinia-bringup/spec.md`：通过本 change 的 delta 增加 call-aware builtin/barrier contract。
- `openspec/specs/replicated-scalar-state/spec.md`：预计无需 delta，除非实现触碰 scalar-state lowering 语义。
