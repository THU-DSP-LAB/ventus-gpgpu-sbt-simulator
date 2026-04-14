## Implementation Tasks

- [x] 1. 在 `sbt/ptx_emit.cpp` 的 `EmitCtx` 中引入按类型分配的虚拟临时寄存器分配器，并支持函数级 `.reg` 声明汇总输出。
- [x] 2. 保留当前固定语义寄存器槽位不变，显式整理并注释哪些 `%r/%rd/%p` 仍属于 machine/runtime/control contract，哪些 helper 改为通过 `%tmp*` 申请 scratch。
- [x] 3. 先迁移高风险 scratch 路径到 `%tmp*`：地址映射 helper、scalar FP lowering、builtin lowering、direct-call marshal 周边临时值；确保这些路径不再依赖共享固定 scratch 编号。
- [x] 4. 继续迁移 custom non-MMA 与 MMA 相关 scratch 路径，消除剩余“默认占用 `%r14/%r15/...`”式隐式 scratch 约定。
- [x] 5. 补充或更新自动化验证，至少覆盖 PTX compile-first、现有 emitter/unit 回归，以及对生成 PTX 中 `%tmp*` 声明/使用合法性的检查。
- [x] 6. 同步文档：更新 `README.md`、`doc/README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`openspec/README.md`，明确 current/active 口径下的固定槽位与 `%tmp*` scratch 策略。
- [x] 7. 完成最终一致性检查：核对 `openspec/specs/`、change artifacts、README/doc 索引中的 `current` / `active` / `historical` / `legacy` 标注与引用口径一致，避免旧的固定 scratch 池表述继续被当作 current contract。
