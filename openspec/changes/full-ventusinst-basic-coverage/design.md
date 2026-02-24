## Context

本仓库的核心链路是：Ventus RISC-V ELF（由 Ventus PoCL/clang 编译 OpenCL C 得到）→ `sbt_decode` 解码/CFG verify → `sbt_ptx` 发射 PTX → `ptxas` 编译/driver JIT → `VENTUS_BACKEND=ptx` 端到端运行。

Spike 路径通过 Ventus 软件栈的 `VENTUS_BACKEND=spike` 提供功能仿真，可作为语义 oracle。当前 Ventus 软件栈不支持 `clCreateProgramWithBinary`（无法从二进制直接 build program），因此对比策略需要基于“同源码输入 + 同编译器链路”，假设编译产物一致。

## Goals / Non-Goals

**Goals:**
- 覆盖 `VentusInst_basic.txt` 中的全部指令 mnemonic（除显式例外），达到 compile-supported。
- 为全部指令提供 Spike-vs-PTX 的微测例覆盖（A/B buffer 写回对比），达到 semantic-supported（浮点允许近似）。
- 形成稳定的“例外列表”机制：除 `jalr` 非 `ret` 形态外，新增例外必须显式记录并告知确认。

**Non-Goals:**
- 不要求引入新的“扫描/报表型”诊断子命令来输出缺失指令的具体名字清单（覆盖工具只需输出统计与 gate 结果）。
- 不要求浮点 bit-accurate；只要求语义一致/数值近似。
- 不在本变更中支持间接跳转/间接调用（`jalr` 非 `ret` 形态）。

## Decisions

### 1. 测试 oracle：同一 host 程序两次运行（`VENTUS_BACKEND` 切换）

使用同一份 OpenCL 源码微测例，通过 host 程序分别在：
- `VENTUS_BACKEND=spike`
- `VENTUS_BACKEND=ptx`
运行并读回输出 buffer B，对比输出字节流。

理由：
- 不依赖 `clCreateProgramWithBinary`。
- 与 Ventus 软件栈/driver 的实际运行方式一致（真实走 PoCL + driver ABI）。

**Alternatives considered:**
- `clCreateProgramWithBinary`：当前 Ventus driver 不支持 binary build，无法落地。
- 在同一进程创建两个 OpenCL device/platform：Ventus 软件栈使用单 device，通过环境变量切换后端，不适用。

### 2. 覆盖统计：基于 `sbt_decode --json` 的“被触达 mnemonic 集合”

对每个微测例 kernel：
1) 编译生成 `.riscv`（由 Ventus 软件栈完成）
2) 对该 kernel 用 `sbt_decode decode ... --require-known --json` 导出解码结果
3) 收集 `decoded[].name` 集合，与 `VentusInst_basic.txt` 的 mnemonic 集合求交，输出覆盖统计

覆盖门槛（gate）先从“统计输出”开始，后续逐步提升为“全覆盖（除例外）”。

**Alternatives considered:**
- 静态从 `VentusInst_basic.txt` 生成测试/实现任务清单：可作为辅助，但最终仍需以实际编译产物的指令流为准。

### 3. 实现策略：按“指令族”推进，并逐步表驱动化

单条指令 case-by-case 在 bring-up 阶段可行，但全表覆盖需要降低一致性风险：
- 将 decode 分类规则（寄存器类、立即数类、load/store/branch 分类）集中化、可测试化。
- emitter 侧尽量复用统一 helper（地址映射、符号/零扩展、lane 操作），避免重复实现导致语义漂移。
- 对 RVV/Ventus 指令族采用“同一族的统一 lowering 形态”，减少字符串 if 链膨胀。

### 4. 浮点语义：允许近似，对比采用 tolerance

微测例比较规则：
- 整数/位运算：逐元素完全一致。
- 浮点：`abs(a-b) <= atol + rtol*abs(a)`，并对 NaN/Inf 做一致性规则。

### 5. 例外列表：小且显式

默认允许的例外仅：
- `jalr` 非 `ret` 形态（间接跳转/调用）

新增例外需要：
- 在 spec 的 Exceptions 中增加条目与理由；
- 覆盖统计/测试输出中显示例外数量（不需要列出缺失指令名清单）。

## Risks / Trade-offs

- 全表覆盖会暴露更多“寄存器/类型/地址空间细节”的一致性问题，需要更强的表驱动与共用 helper 才能稳定推进。
- 微测例数量增大可能拉长回归时间；需要控制 NDRange（例如 1 block×1 warp）并支持按组运行。
- RVV/Ventus 的跨 lane/permute/reduction 指令可能需要 shared/shuffle 实现，复杂度高，应在任务拆分中单独成组推进。

