# 2026-04-12 临时接手记录：`m16n16*` MMA 在 Spike 预检中的现状与已验证结论

- 状态：active temporary note
- 日期：2026-04-12
- 关联 change：`support-custom-mma`
- 用途：给下一位接手者快速复用这次排查结论，避免重复踩坑

## 1) current

截至本次记录，当前树下可稳定复现：

- `python3 tools/custom_mma_oracle.py --stage spike-precheck --sm 89 --n 64`
- `bash tools/regress.sh --preset all`

结果一致为：

- `m16n8k16 f16->f32`：PASS
- `m16n8k16 bf16->f32`：PASS
- `m16n8k8 tf32->f32`：PASS
- `m16n16k16 f16->f32`：FAIL
- `m16n16k16 bf16->f32`：FAIL
- `m16n16k8 tf32->f32`：FAIL
- `fp16->fp16 blocked`：仍按 blocked probe 保留

失败表象仍是 `spike-precheck` 下这 3 个 `m16n16*` kernel 输出全 0。

## 2) 这次已确认的关键结论

### current：第一层问题是多-kernel 源文件入口误选

最初复现时，虽然请求执行的是 `mt_custom_mma_m16n16*`，但 Ventus PoCL 编译日志里传给 linker 的 `--init` 仍是源码中第一个 kernel：

- `-Wl,--init=mt_custom_mma_m16n8k16_row_col_f32_f16_f16_f32`

因此第一层问题并不是 `m16n16*` lowering 本体，而是 Ventus PoCL 在多-kernel OpenCL 源文件上可能把首个 kernel 错当成入口。

### current：这一层已在仓库内局部绕开

本轮唯一保留的代码修复是：

- `tools/custom_mma_oracle.py` 现在对 `m16n8k16 f16->f32` 也显式加 `source_define`
- `testcases/ocl_compare/custom_mma_kernels.cl` 中 `m16n8k16 f16->f32` 也受 `SBT_MMA_ENABLE_F16_M16N8K16` 宏控制

这样 oracle 每次都会 materialize 只含目标 kernel 的单-kernel 源文件，绕开 PoCL 的多-kernel 入口误选问题。

当前保留修改：

- [tools/custom_mma_oracle.py](/work/ventus-env-torch/sbtsim/tools/custom_mma_oracle.py:113)
- [testcases/ocl_compare/custom_mma_kernels.cl](/work/ventus-env-torch/sbtsim/testcases/ocl_compare/custom_mma_kernels.cl:11)
- [tools/README.md](/work/ventus-env-torch/sbtsim/tools/README.md:20)
- [doc/IMPLEMENTATION_CODEMAP.md](/work/ventus-env-torch/sbtsim/doc/IMPLEMENTATION_CODEMAP.md:166)

### current：入口修正后，第二层问题仍然存在

在单-kernel 源文件下再次直接执行 `m16n16*` kernel，编译日志里的 `--init` 已经变成正确目标：

- `mt_custom_mma_m16n16k16_row_col_f32_f16_f16_f32`
- `mt_custom_mma_m16n16k16_row_col_f32_bf16_bf16_f32`
- `mt_custom_mma_m16n16k8_row_col_f32_tf32_tf32_f32`

但这 3 个 kernel 仍在 `spike-precheck` 下全零失败。

### current：`m16n16*` builtin 并不是完全没生效

我做过最小探针，结论是：

- 生成的 `object0.riscv` 里确实能看到 `m16n16*` 的函数符号与对应 `mma.m16n16*...` 指令
- `m16n16k16 f16->f32` 的 builtin 返回结果本身不是全零
- 若在单线程里把整个 `float8 d` 一次性写回全局内存，可以观察到非零结果

因此不能把问题简单归咎为：

- “编译器没发出 `m16n16*` 指令”
- “Spike 对 `m16n16*` 一律返回零”

### current：更像宽结果 `float8` 的 lane extraction / codegen 问题

关键隔离结论：

- 不含 `mma`、只构造 `float8` 常量并按 `v.s0..v.s7` 读出时，可以正常得到非零输出
- 一旦在 `m16n16*` builtin 返回的 `float8 d` 上做逐 lane 提取，原始 microtest 会表现为全零
- 我尝试把 `m16n16*` microtest 改成“每 8 个线程由 leader 一次性把 `d.s0..d.s7` 写回”
- 该改法在独立最小探针里能编过并看到非零结果
- 但回灌到正式 microtest 后，Ventus clang 在 `RISCV DAG->DAG Pattern Instruction Selection` 直接崩溃，`rc=-6`

因此当前剩余 blocker 更像：

- Ventus LLVM/clang 对 `m16n16*` 宽结果 `float8` 的读取/提取代码生成有问题
- 或者 `m16n16*` builtin 返回 carrier 与普通 `float8` 的后续提取 contract 未对齐

而不再优先像 sbtsim 内部的 PTX lowering 公式问题。

## 3) 这次做过的尝试

### 尝试 A：单-kernel materialize

动作：

- 给 `m16n8k16 f16->f32` 补 feature macro
- 让 MMA oracle 每次只暴露一个目标 kernel

结果：

- 有效，确认并绕开了 PoCL 多-kernel 入口误选问题
- 该尝试已保留在当前工作树中

### 尝试 B：把 `m16n16*` 写回从逐 lane 提取改成 8-lane leader 一次性写回

动作：

- 在 `custom_mma_kernels.cl` 中把 `m16n16*` 的 `B[gid] = as_uint(pick_float8_lane(d, gid))`
- 改成 `(gid & 7u) == 0` 时直接写 `d.s0..d.s7`

结果：

- 在独立 probe 里可编过并得到非零结果
- 但在正式 microtest 上会触发 Ventus clang 后端崩溃，位置为：
  - `RISCV DAG->DAG Pattern Instruction Selection`
  - 函数为对应 `mt_custom_mma_m16n16*`

处理：

- 该尝试已完全回退
- 不要以当前主树状态继续沿这条具体改法推进

## 4) 对下一位接手者最有价值的经验

### 不要再从“是不是还在跑第一个 kernel”开始重复排查

这层已经确认过，也已经在当前树里做了 repository-local 绕开。

### 优先把问题视作工具链宽结果 ABI / codegen 问题

推荐下一步优先级：

1. 继续做最小 probe，而不是直接改正式 microtest
2. 重点围绕 `m16n16* builtin -> float8 result -> lane extraction/store` 这条链路
3. 把“普通 `float8` 正常、`m16n16* builtin` 结果提取异常”的差异最小化到最短 OpenCL 片段
4. 若需要真正修复，大概率会落到 `../llvm` 或其安装产物，而不是本仓库 `sbt/*`

### 不要把现象误判成 PTX side 问题

这次没有证据支持：

- `sbt_decode` 把 `m16n16*` decode 错了
- `sbt_ptx` 没发出 `m16n16*` lowering
- `m16n16* split-n` 在 PTX compare 阶段先坏了

当前甚至还没稳定越过 Spike 预检，就卡在前端/编译期或宽结果提取行为上。

## 5) 当前工作树状态

本次应保留的修改：

- `tools/custom_mma_oracle.py`
- `testcases/ocl_compare/custom_mma_kernels.cl`
- `tools/README.md`
- `doc/IMPLEMENTATION_CODEMAP.md`

这些修改只涉及“单-kernel materialize 绕开入口误选”。

本次未触碰但当前树里存在的额外脏改动：

- `tools/ventus_ocl_run.cpp`

本次未处理的额外未跟踪项：

- `TMP_INPUT_PLACEHOLDER`
- `issue_addmm_error/`

接手时不要把这些误认为本 note 对应修改的一部分。

## 6) 当前建议命令

复现当前状态：

```bash
python3 tools/custom_mma_oracle.py --stage spike-precheck --sm 89 --n 64
bash tools/regress.sh --preset all
```

若继续做最小探针，优先在临时目录中单独生成最短 `.cl`，不要直接污染正式 microtest。
