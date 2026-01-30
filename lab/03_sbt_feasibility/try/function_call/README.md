# 函数调用语义识别（可行性验证）

目标：以 `ventus-env/rodinia/opencl/*/*.riscv` 作为输入，基于同路径的 `*.dump` 反汇编，做最小化的语义识别验证：
- 函数划分（来自 ELF `.symtab` 的 `STT_FUNC`）
- 直接调用（`jal` 写 `ra` 且目标为函数入口）
- 尾调用候选（`j`/`jal x0` 跳到函数入口）
- 返回（`ret` 或 `jalr x0, ra, 0`）

## 运行

在仓库根目录执行：

```bash
python3 lab/03_sbt_feasibility/try/function_call/analyze_calls.py
```

输出写入：
- `lab/03_sbt_feasibility/try/function_call/out/REPORT.md`
- `lab/03_sbt_feasibility/try/function_call/out/all.json`
- `lab/03_sbt_feasibility/try/function_call/out/*.json`（每个对象一份）

## 说明

- 函数边界：优先用 `.symtab` 的 `Value/Size`；`Size=0` 的函数用“下一个函数入口地址”估算结束位置（仅用于把指令归属到 caller）。
- 本工具只做“可行性验证”，不尝试处理 `jalr` 的函数指针调用/跳表等复杂情形；这些会被归类到 `indirect_calls/indirect_jumps` 或 `unknown_control_flow` 以便后续补全。

