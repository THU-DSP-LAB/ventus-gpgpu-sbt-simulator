## Why

当前 SBT 生成的多函数 PTX 在存在“前向调用”（caller 先出现，callee 后定义）时，CUDA JIT 可能报错：
- `Call to '<func>' requires call prototype`
- `Unknown symbol '<func>'`

这会导致 `cuModuleLoadDataEx(JIT)` 失败，进而在运行时触发内核启动断言失败。该问题不是单个测例逻辑错误，而是 PTX 模块组织方式不满足 ptxas 对调用原型可见性的要求。

## What Changes

- 在 PTX 模块头部为所有 SBT 生成的可调用 `.func` 统一输出前置 prototype（函数签名与实际定义保持一致）。
- 保持现有 `.func` 与 `.entry` 的发射顺序和调用逻辑不变，仅补齐原型可见性约束。
- 增加针对“前向调用”场景的最小化回归测试，确保不再出现 prototype/unknown symbol 类 JIT 失败。
- 更新文档，明确多函数 PTX 的调用约束与当前实现策略。

## Capabilities

### New Capabilities
- **ptx-call-prototype-declaration**：SBT 发射的 PTX 在模块级别具备完整的被调函数 prototype 声明，支持前向调用被 ptxas 正确解析。

### Modified Capabilities
- **ptx-module-emission**：多函数模块从“依赖定义顺序隐式满足调用解析”调整为“显式声明 prototype 后再发射函数体”。

## Impact

- 正向影响：消除因函数定义顺序导致的 JIT 不稳定，降低 `cuModuleLoadDataEx` 失败率。
- 兼容性：不改变现有 ABI、参数布局与调用协议，仅增加声明段。
- 维护性：将“调用可见性”从隐式顺序约束转为显式模块约束，便于后续扩展更多 helper 函数。
