## Implementation Tasks

- [x] 1. 冻结 emitter 模块化边界：盘点 `sbt/ptx_emit.cpp` 中 shared core 责任、domain-specific lowering 责任，以及当前 dispatch precedence；把不应迁出的 ABI/helper/core contract 明确列出，避免后续按行数硬拆。
- [x] 2. 引入 emitter internal shared interface：为 shared host（`EmitCtx` 角色）和 domain-level `try_emit_*` 入口建立内部声明边界，使多个 lowering translation units 可以复用同一宿主状态与 core primitives，同时保持 `sbt/ptx_emit.hpp` public API 不变。
- [x] 3. 将 module/function assembly、固定寄存器与 blob ABI、address-mapping helpers、temp/label allocation、prologue/epilogue、shared precondition validation、CFG block traversal/fallthrough emission 收敛到 shared emitter core，避免这些 contract 在新 lowering 单元中复制一份。
- [x] 4. 将 `emit_one_inst()` 收敛为薄 dispatcher，显式固化 domain precedence，并把 comments / scalar-exec metadata gate / shared fail-fast preconditions 保持在 dispatcher-core 边界统一执行；同时保持 `custom.family == Mma` 等当前 ownership 规则不会在重构中被 generic custom 路径吞掉。
- [x] 5. 拆出 `control` lowering 单元，覆盖 structured control、ret、direct call、builtin call lowering、branch、CSR；确认 builtins 继续归属于 call/control resolution，而不是按最终 PTX 形态分散到 scalar/vector 单元。
- [x] 6. 拆出 `scalar` lowering 单元，覆盖 scalar memory、scalar integer、scalar FP，并确保其继续通过 shared host 复用 current scalar-state / temp / address-mapping contract，而不是复制 helper。
- [x] 7. 拆出 `vector` lowering 单元，覆盖 vector memory、vector register、vector integer、vector floating-point、compare、convert、mask；首阶段保持该域内部完整，不为了进一步压缩单文件行数而过早细拆。
- [x] 8. 拆出 `custom` lowering 单元，仅承载 custom non-MMA；拆出 `mma lowering` 单元，用于 MMA PTX materialization，并继续复用现有 `sbt/ptx_mma.*` planner/ABI helper，不把 MMA 再退化回混入 generic custom 或 core 的路径。
- [x] 9. 更新构建与验证入口：调整 `CMakeLists.txt`、相关内部头/源文件编译关系，并同步扩展 `tools/check_ptx_emit_name_allowlist.py` 与相关 structural checks，使其覆盖完整 emitter lowering 文件集而不是只扫描单一 `sbt/ptx_emit.cpp`。
- [x] 10. 运行并补强回归：至少覆盖 `ptx_emit_call_prototype_test`、`ptx_emit_leader_lane_abi_test`、`custom_ptx_emit_test`、`mma_ptx_emit_test`、`external_mnemonic_contract_test`、compile-first 路径，以及模块化后仍需成立的 authority/allowlist 检查。
- [x] 11. 同步文档：更新 `README.md`、`doc/README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`openspec/README.md`、`tools/README.md`，把 emitter 的 current as-built 结构与本 active change 的目标结构区分清楚。
- [x] 12. 做最终一致性检查：确认实现没有演变成“为拆文件而拆”、没有新增 fallback/guardrail、没有削弱现有 authority/validation contract，并校对所有相关文档中的 `current` / `active` / `historical` / `legacy` 标注保持一致。
