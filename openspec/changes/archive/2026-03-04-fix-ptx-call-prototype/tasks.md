## Implementation Tasks

- [x] 在 `sbt/ptx_emit.cpp` 为 helper `.func` 生成统一前置 prototype 声明，确保位于所有函数体之前。
- [x] 复用/集中 helper 参数签名发射逻辑，确保 prototype 与 `.func` 定义签名严格一致（含 `need_vctx` 分支）。
- [x] 增加或更新针对前向调用场景的回归测试，验证不会再出现 `requires call prototype` / `Unknown symbol` 组装错误。
- [x] 同步更新相关文档（至少 `README.md` 与 `doc/` 对应文档），说明多函数 PTX prototype 策略与验证方式。
