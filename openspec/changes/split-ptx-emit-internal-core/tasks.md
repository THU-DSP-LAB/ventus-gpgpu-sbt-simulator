## Implementation Tasks

- [x] 1. 建立基线清点：记录当前 `sbt/ptx_emit_internal.hpp` 中仍承载的实现块，按 core/function assembly、runtime/PDS、memory/address mapping、call ABI、builtin、scalar FP、MMA materialization 分类，并确认哪些 helper 必须保留为 shared `EmitCtx` interface。
- [x] 2. 新增空实现单元并接入构建：添加计划中的 `ptx_emit_core.cpp`、`ptx_emit_runtime.cpp`、`ptx_emit_memory.cpp`、`ptx_emit_call.cpp`、`ptx_emit_builtin.cpp`、`ptx_emit_scalar_fp.cpp` 等需要的 `.cpp`，更新 `CMakeLists.txt`，先保持无行为变化并完成一次构建。
- [x] 3. 收敛 internal header interface：将 `sbt/ptx_emit_internal.hpp` 调整为字段、常量、small inline primitive 与 member/free-function declarations；避免一次性删除所有实现，按后续迁移 slice 逐步缩小 header。
- [x] 4. 迁移 core/function assembly：把 register declarations、function header/body assembly、shared precondition validation、dispatcher、CFG block traversal、fallthrough emission 等 out-of-line 到 core implementation，保持 dispatcher precedence 为 `control -> scalar -> vector -> mma -> custom`。
- [x] 5. 迁移 memory/address mapping：把 numeric address mapping、typed load/store helper、leader-only scalar store wrapper 移到 memory implementation；确认 scalar/vector lowering 继续复用同一 shared address contract，没有复制本地地址窗口逻辑。
- [x] 6. 迁移 call ABI：把 helper prototype/definition signature、call parameter layout、mutable-state blob marshal、direct-call outgoing machine/runtime blob marshal、direct `.func` call emission 移到 call implementation；确认 helper prototype/definition/value ABI 输出与当前测试期望一致。
- [x] 7. 统一 builtin lookup 与 dispatch：在 builtin implementation 中建立单一 builtin lookup source，使 public `is_inlined_builtin_call_name()` 与 control lowering 的 inline dispatch 共享同一 lookup；增加结构性检查或 focused unit assertion，枚举所有当前 accepted builtin symbol 并证明每个 symbol 都映射到可 dispatch 的 emitter path。
- [x] 8. 迁移 scalar FP：把 FP rounding normalization、`fclass`、scalar FP lowering body 移到 scalar FP implementation；保持 `rm=DYN` note、unsupported rounding fail-fast 与 Zfinx raw-bit behavior 不变。
- [x] 9. 迁移 MMA materialization：把 MMA metadata validation、tuple materialization、shuffle gather/merge、native/composite MMA emission 移到 `ptx_emit_mma_lowering.cpp` 或同等 MMA ownership file；继续复用 `sbt/ptx_mma.*` planner/ABI helper，不复制 tuple planner contract。
- [x] 10. 迁移 runtime/PDS：把 entry/helper prologue、PDS acquire/release、CSR/PDS runtime helpers 移到 runtime implementation；重点校验 PDS acquire/release 时序、shared state 和 trap/fail-fast 行为不变。
- [x] 11. 每个行为迁移 slice 后立即构建并运行对应 focused check：core/dispatch 后跑 metadata/external contract，memory/runtime 后跑 leader-lane 或 quick PDS 相关 check，call/builtin 后跑 call prototype 与 builtin 同步检查，custom/MMA 后跑 custom/MMA emit tests；不要把所有验证推迟到最终大 diff。
- [x] 12. 清理 includes 与 declarations：移除 internal header 中不再需要的 heavy includes，将具体 `.cpp` 所需 include 下沉到本文件；确保 domain lowering files 只通过必要 interface 编译。
- [x] 13. 更新结构性检查：扩展 `tools/check_ptx_emit_name_allowlist.py` 的 emitter file set，覆盖所有新增/迁移后的 implementation files；增加 builtin lookup/dispatch 同步检查，避免 allowlist 接受但 dispatch 缺路径。
- [x] 14. 运行 focused regression：至少运行 `external_mnemonic_contract_test`、`instruction_metadata_contract_test`、`ptx_emit_call_prototype_test`、`ptx_emit_leader_lane_abi_test`、`custom_ptx_emit_test`、`mma_ptx_emit_test` 与 `python3 tools/check_ptx_emit_name_allowlist.py`。
- [x] 15. 运行 compile-first / quick gate：至少覆盖一个 `sbt_ptx --require-known` + `ptxas -arch=sm_89` compile-first 示例；条件允许时运行 `tools/regress.sh --preset quick --arch sm_89`。
- [x] 16. 同步文档：更新 `README.md`、`doc/README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`openspec/README.md`、`tools/README.md`，明确区分 current as-built、active change 目标、historical `2026-04-25-modularize-ptx-emit-lowering`。
- [x] 17. 同步 current spec：实现完成并验证通过后，将本 change 的 delta 同步到 `openspec/specs/ptx-lowering-modularity/spec.md`，再按归档流程处理该 active change。
- [x] 18. 最终一致性自审：检查是否改变 public API、dispatcher precedence、ABI/register/temp/address/PDS/MMA/name-allowlist contract；检查是否出现 helper copy、并列 active 文档入口、未来时描述已落地事实、或 current/active/historical/legacy 状态冲突。
