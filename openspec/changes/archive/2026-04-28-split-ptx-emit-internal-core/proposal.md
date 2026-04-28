## Why

当前 PTX emitter 已完成一轮 current 模块化收敛：

- `sbt/ptx_emit.cpp` 现在只承担 public API、`EmitError` 与 module assembly；
- lowering 已按 `control / scalar / vector / custom / mma` translation units 分离；
- supported correctness path 继续消费 `DecodedInst.emit` / `DecodedInst.custom` / `DecodedInst.mma`，`DecodedInst.name` 只保留 external/comment/diagnostic 等 allowlist 用途；
- `tools/check_ptx_emit_name_allowlist.py` 已覆盖当前 emitter 文件集。

但这个结构仍有一个明显的后续维护风险：`sbt/ptx_emit_internal.hpp` 已经成为新的实现集中点。它当前同时包含 shared host interface、`EmitCtx` 状态、寄存器和 temp helper、PDS acquire/release、地址映射、call blob ABI、builtin lowering、scalar FP lowering、MMA materialization、dispatcher 与 function body assembly。问题不是“header 行数”本身，而是：

- internal interface 与 implementation 混在一个 header 内，任何 lowering 单元都被迫看到大量无关实现细节；
- `ptx_emit_mma_lowering.cpp` 等文件名表达的 ownership 与真实实现位置不完全一致；
- builtins、call ABI、address mapping、runtime/PDS、MMA shuffle materialization 等高风险 contract 仍难以单独 review；
- 后续继续扩 scalar/vector/custom/MMA 时，容易把新实现继续塞进 internal header，导致已完成的 domain modularity 退化；
- header 中大量非模板、非必要 inline 实现会扩大 rebuild 和 include 耦合面。

因此本 change 是已归档 `modularize-ptx-emit-lowering` 的后续增量：不重新定义 emitter 的语义域，不改变 public API，也不改变 current PTX lowering 行为；只把 `ptx_emit_internal.hpp` 收敛为真正的 internal shared interface，并把大块实现迁回职责明确的 `.cpp` 单元。

## What Changes

- 将 `sbt/ptx_emit_internal.hpp` 收敛为 internal declarations / small inline primitives / shared `EmitCtx` interface，而不是继续承载大段 implementation。
- 将当前仍滞留在 internal header 中的实现按实际 ownership 迁到 `.cpp`：
  - core/function assembly：function body assembly、fixed/virtual temp declarations、dispatcher、fallthrough emission；
  - runtime/PDS：entry/helper prologue、PDS pool acquire/release、machine/runtime CSR helpers；
  - memory/address mapping：shared/global numeric address mapping、typed load/store helper、leader-only scalar store wrappers；
  - call ABI：helper signature、mutable/machine/runtime blob marshal、direct call emission；
  - builtin lowering：builtin allowlist 与 builtin inline dispatch；
  - scalar FP：`fclass`、FP rounding normalization、scalar FP lowering;
  - MMA lowering：MMA PTX materialization、tuple shuffle gather/merge、native/composite lowering validation。
- 保持现有 domain entrypoints 和 dispatcher precedence：`control -> scalar -> vector -> mma -> custom`。
- 保持 `sbt/ptx_emit.hpp` public API、`emit_module()` / `emit_kernel()` 行为、value ABI、register ownership、address-space contract、MMA contract、name allowlist contract 不变。
- 更新构建入口与结构性检查，使新增/迁移后的 emitter implementation files 继续纳入 allowlist 和 regression 覆盖。
- 同步文档，把 `current` as-built 结构、这个 `active` change 的目标结构、以及历史 `modularize-ptx-emit-lowering` 的已归档状态区分清楚。

本 change 明确不做：

- 不新增 PTX 指令支持面；
- 不改变 descriptor/payload lowering authority；
- 不引入 registry/plugin/virtual handler tree；
- 不拆分 `cfg` / `cfg_verify`；
- 不为了文件行数平均而拆碎紧耦合 helper；
- 不直接修改上级 `ventus-env` 子项目源码。

## Capabilities

### New Capabilities

- 新增 **ptx-emitter-internal-core-boundary**：PTX emitter 的 internal header 只暴露共享 host/interface contract，大块实现按 ownership 落到 `.cpp`。
- 新增 **ptx-emitter-implementation-ownership-map**：runtime/PDS、memory/address mapping、call ABI、builtin、scalar FP、MMA materialization 等内部实现拥有明确维护边界。

### Modified Capabilities

- 修改 **ptx-lowering-modularity**：在现有 domain lowering modularity 基础上，进一步要求 shared internal interface 不再成为新的 monolithic implementation container。
- 修改 **PTX emitter structural validation**：allowlist / regression / documentation checks 必须覆盖拆分后的完整 internal implementation file set，而不是只覆盖 domain lowering `.cpp` 与 internal header 的旧组合。

## Impact

正向收益：

- 后续 review 可以按 runtime、memory、call ABI、builtin、scalar FP、MMA 等高风险 contract 聚焦，而不必在 `ptx_emit_internal.hpp` 中跨 1900+ 行定位。
- `ptx_emit_mma_lowering.cpp`、builtin/call 等文件名与真实 implementation ownership 对齐，降低维护误判。
- 收窄 header implementation 暴露面，避免 include 耦合面继续扩大，使 domain lowering 单元只依赖必要 interface。
- 防止当前 internal header 继续吸纳新功能，保护已完成的 lowering modularity 成果。

主要风险：

- 若拆分过程中移动 helper 时改变了固定寄存器、temp allocation、blob ABI、address mapping 或 PDS release 时序，会引入行为回归。
- 若把 core primitives 复制到多个 `.cpp`，会把 monolith 风险变成多处 contract 漂移。
- 若 builtin allowlist 与 builtin dispatch 分离后不同步，direct-call closure 与 inline lowering 可能出现不一致。
- 若 static checks 未同步扫描新增文件，`DecodedInst.name` allowlist 或 descriptor authority regression 可能漏检。

Documentation Impact:

- `README.md`
- `doc/README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`
- `openspec/README.md`
- `tools/README.md`

Superseded / Related Notes:

- 本 change 不 supersede 已归档的 `openspec/changes/archive/2026-04-25-modularize-ptx-emit-lowering/`；它是该 current 结果上的后续 active refinement。
- 后续关于 “是否还要继续拆 `ptx_emit_internal.hpp`” 的讨论应以本 change 为 active 入口。
