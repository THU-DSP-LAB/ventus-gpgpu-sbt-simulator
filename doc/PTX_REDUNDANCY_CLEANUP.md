# PTX 冗余消除

> 状态：`current`

`EmitCtx::emit_body()` 在完成 lowering 后、生成寄存器声明前调用 `sbt/ptx_cleanup.cpp`。entry 与 direct-call helper 使用同一清理路径，ISA metadata 与 ABI 仍由原 emitter 负责。

## 已消除的冗余

| 审查路径 | 原冗余 | 当前处理 |
| --- | --- | --- |
| vector/scalar FP、custom SFU/conversion、builtin | 同宽 `.b32` ↔ `.f32` 输入/输出拷贝 | 传播 bit-size 源，合并相邻结果写回 |
| scalar integer、vector scalar operand、地址映射 | `%x/%v` 到 scratch 的拷贝链 | 源未被重定义时直接使用源寄存器，删除死 scratch |
| 整数算术/位操作 | 加减零、移位零、低位乘一、AND 全位掩码 | 转为同宽 `mov`，继续传播并消除自拷贝 |
| 控制流 | 无条件跳到下一组连续标签 | 删除分支并保留全部标签 |
| entry prologue | legacy shared scalar backing 地址计算，随后清零且无读取方 | 删除计算与无用 metadata 读取 |
| scalar FCLASS | 六条旧固定谓词计算，与当前结果无关 | 从生成源删除 |
| 函数声明 | 清理后未使用的 virtual temp、完全未使用的 fixed group | 不生成声明；保留已使用 fixed group 的编号与尺寸 |

例如：

```ptx
// before
mov.b32 %tmp_f32_2, %v6;
ex2.approx.f32 %tmp_f32_3, %tmp_f32_2;
mov.b32 %v6, %tmp_f32_3;

// current
ex2.approx.f32 %v6, %v6;
```

PTX bit-size 类型兼容同宽的操作数类型，因此 `.b32` 寄存器可以直接用于 `.f32` 指令。依据：[NVIDIA PTX operand type checking](https://docs.nvidia.com/cuda/archive/12.2.2/parallel-thread-execution/index.html#operand-type-checking)。

## 正确性条件

- 仅建立同宽、无谓词的 bit-size 源别名；源/目标重定义（包括有谓词的重定义）立即使别名失效。按完整寄存器 token 替换。
- `.f32` scratch 不直接作为整数操作的替换源；输出可通过修改 producer 目的寄存器写入同宽 bit-size 寄存器。
- producer/writeback 必须相邻（可间隔空行/注释）、谓词相同、结果在写回后不再活跃。
- dead scratch 分析保留跨段引用和段内先读后定义的循环携带值。相同谓词的定义只覆盖同谓词读取；谓词重定义会使此事实失效。
- `.cc`、控制流、call、warp collective、MMA、同步、原子和未分类操作保持传播边界。load/store 可以参与操作数替换，但不删除内存操作；trap 保留。
- 不消除浮点加零/乘一、窄化或数值转换，不合并 half unpack/pack。
- `%x/%v` 和固定 machine/runtime/control 状态不按死 scratch 删除。清理采用局部证明，不执行跨块全局 DCE 或 ABI 状态裁剪。

## 验证与维护

- `build/ptx_cleanup_test`：拷贝、重定义、谓词、循环、类型、token、副作用与幂等性。
- `build/custom_ptx_emit_test`：真实 in-place EX2 emitter 输出及 `ptxas`。
- `tools/regress.sh --preset all --arch sm_89`：C++ 合同测试、custom MMA/non-MMA oracle、Rodinia/PDS compile-first、Spike/PTX 微测例和 PoCL/driver 端到端。
- 新增 PTX 操作时需确认是否纯计算、目的操作数形式和隐含状态，再决定是否加入清理分类；未分类操作保留。
- PTX 指令减少不等于 SASS 数量或 GPU 执行时间改善；`ptxas` 也可能已经消除相应拷贝。

实施与回归记录见 [工作日志](../worklog/20261011-PTXRedundancyOptimization.worklog.md)。
