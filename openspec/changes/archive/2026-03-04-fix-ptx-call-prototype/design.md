## Context

`sbt_ptx` 会生成一个包含 `.entry` 与多个 helper `.func` 的 PTX 模块。当前实现在 helper 之间存在直接 `call.uni`，但没有统一的前置 prototype 声明，导致当 caller 在文本上先于 callee 时，ptxas 在单遍解析时可能报：
- `Call to '<func>' requires call prototype`
- `Unknown symbol '<func>'`

本变更目标是消除这类“定义顺序敏感”的 JIT 失败，同时保持现有 ABI 和调用语义不变。

## Goals / Non-Goals

**Goals:**
- 在模块级提供完整 helper 调用原型，支持前向调用。
- 保持现有 `.func` 定义体、`call.uni` 参数布局、符号命名规则不变。
- 增加可复现的最小回归覆盖，验证“前向调用不再失败”。

**Non-Goals:**
- 不重写 call graph 排序策略。
- 不引入新的 fallback/静默降级路径。
- 不改变 kernel/driver ABI、参数数量或参数类型。

## Decisions

### 1. 在模块头统一发射 helper prototype 声明

在 `.extern .shared ...` 之后、函数体之前，先输出每个 helper 的 `.func` prototype（仅签名，不含 body）。之后按既有逻辑输出 helper 定义与 entry 定义。

这样可将“调用可见性”从“依赖定义顺序”转为“显式声明”，满足 ptxas 要求，且实现改动最小。

**Alternatives considered:**
- 调整 helper 定义顺序（callee 在前 caller 在后）: 拒绝。需要严格拓扑排序并处理更多边界，且未来新增 helper 后仍可能因为局部顺序问题回归。
- 将部分 helper 改为内联: 拒绝。会扩大代码改动面并影响可读性与调试定位，不是问题根因修复。

### 2. 复用统一签名生成逻辑，避免声明/定义漂移

prototype 和 `.func` 定义必须严格同构。实现时采用共享的参数发射逻辑（或等价的集中化字符串构造）确保两处一致，避免后续参数变更时一处遗漏。

**Alternatives considered:**
- 手写两份参数列表: 拒绝。维护成本高，容易出现 ABI 漂移。

## Risks / Trade-offs

- 风险：若 prototype 与定义参数不一致，错误会从“未知符号”变为“签名不匹配”并在组装期暴露。
- 取舍：增加少量 PTX 文本长度，换取更稳定的 JIT 行为与更低排障成本。
- 约束：保持现有符号命名与 call lowering，不改变运行时执行路径。
