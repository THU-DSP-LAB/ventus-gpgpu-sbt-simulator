## Why

当前 `sbt/ptx_emit.cpp` 仍主要依赖一组预留好的 `%r/%rd/%p/%f/%h` 固定编号 scratch 槽位来承载临时值。随着 helper、builtin、地址映射、custom lowering 与 MMA lowering 逐步增多，这种“默认占用某几个编号”的写法有两个结构性问题：

- 临时寄存器的所有权隐式分散在多个 helper 中，维护者需要人工证明不同路径不会误复用同一编号。
- 当某个 helper 被复用、扩展或局部重排时，固定 scratch 约定容易形成“无意覆盖”或“看起来可复用、实际上依赖旧编号布局”的脆弱耦合。

这些问题不属于 PTX 语义错误，但它们明显提高了 emitter 的维护成本和排障成本。当前项目已经把 `%x/%v`、`leader_lane`、machine context、runtime env` 等长期状态与 scratch 寄存器分开建模；临时寄存器层也应进一步显式化，避免把“固定编号池”继续当作默认扩展模式。

本 change 旨在把临时寄存器使用从“手工共享固定编号”收敛到“按类型唯一命名的虚拟临时寄存器”，同时保持当前已冻结的固定语义寄存器槽位与 call ABI 合同不变。

## What Changes

- 在 PTX emitter 中引入按类型分配的虚拟临时寄存器分配器，为 `.b32/.b64/.pred/.f32/.b16/.u8/.u16` 等常用 scratch 类型生成唯一命名的 `%tmp*` 寄存器。
- 将当前临时用途的固定编号 scratch 逐步迁移到 `%tmp*` 分配器管理，重点覆盖地址映射、builtin lowering、标量浮点 lowering、custom lowering 与其它 helper 内部临时值。
- 保留当前带有固定机器语义的寄存器槽位不变，例如 `%r0/%r1/%r2`、machine context 槽位、`%rd0/%rd2/%rd4`、`%x<256>`、`%v<256>`。
- 维持“函数级统一声明 PTX 寄存器”的首阶段实现，不要求在本 change 中引入大量块级 `{}` 局部 scope 或动态交错 `.reg` 声明。
- 更新 current 文档，明确哪些 PTX 寄存器属于固定 machine/runtime slot，哪些属于 `%tmp*` 虚拟临时寄存器。

## Capabilities

### New Capabilities

- **ptx-virtual-temp-register-allocation**：PTX emitter 可以为临时用途生成唯一命名的 `%tmp*` 虚拟寄存器，而不是继续依赖手工共享的固定 scratch 编号。
- **typed-temp-separation**：不同 PTX scratch 类型的临时值可按类型独立分配，避免 `.b32/.b64/.pred/.f32/.b16/.u8/.u16` 临时用途继续混杂在固定编号约定中。

### Modified Capabilities

- **ptx-module-emission**：函数级 PTX `.reg` 声明从“固定 scratch 编号池 + 固定语义寄存器”调整为“固定语义寄存器 + 唯一命名 `%tmp*` 虚拟临时寄存器”。
- **ptx-emitter-maintainability**：helper 对 scratch 寄存器的依赖从隐式编号耦合改为显式临时值申请，降低局部重构时的误覆盖风险。

## Impact

- 正向影响：降低 emitter 内部 scratch 竞争和隐式编号耦合，提升 helper 复用与后续实现迭代的可维护性。
- 兼容性：当前固定语义寄存器槽位、`%x/%v` 逻辑寄存器文件、direct-call value ABI 与 machine/runtime blob 布局保持不变。
- 非目标：本 change 不以“重排所有现有寄存器编号”为目标，也不要求首阶段将 PTX `.reg` 声明细化为大量块级局部 scope。
- 风险：若迁移时漏掉某些 helper 的固定 scratch 依赖，可能造成生成 PTX 仍残留混合模式；因此实现需要配套 compile-first 与现有 emitter 回归检查。

## Documentation Impact

- `README.md`：补充当前 PTX emitter 寄存器使用口径，区分固定 machine/runtime slot 与 `%tmp*` 临时寄存器策略。
- `doc/IMPLEMENTATION_CODEMAP.md`：更新当前 emitter 寄存器约定与 scratch 管理方式。
- `openspec/README.md`：登记新的 active change 入口。
