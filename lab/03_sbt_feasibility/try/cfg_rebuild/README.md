# CFG 重建实验（Ventus ELF）

目标：从 Ventus 二进制 ELF（RISC-V ELF）及其对应的反汇编输出（仓库内现成的 `*.dump`）中解析 `.text` 指令流，按 PC 切基本块并构建 CFG；同时对 `setrpc + vbranch + join` 的 join 点做静态解析，并用后支配/侧出口等规则做正确性验证（对应 `lab/03_sbt_feasibility/summary.md` 的原型阶段约束）。

## 快速开始

以 Rodinia `bfs` 为例：

```bash
python3 lab/03_sbt_feasibility/try/cfg_rebuild/rebuild_cfg.py verify ventus-env/rodinia/opencl/bfs/object0.riscv --emit-dot
```

批量跑 7 个测例（每个目录的 `object0.riscv`）：

```bash
python3 lab/03_sbt_feasibility/try/cfg_rebuild/rebuild_cfg.py verify 'ventus-env/rodinia/opencl/*/object0.riscv' --emit-json
```

输出默认写到 `lab/03_sbt_feasibility/try/cfg_rebuild/out/`。脚本会要求输入 ELF 同目录下存在同名 `.dump`（例如 `object0.riscv` 对应 `object0.dump`），用于识别 Ventus 自定义指令助记符。

## 生成 `.dump`（如缺失）

仓库内 `ventus-env` 已提供可识别 Ventus 自定义指令的 `llvm-objdump`，可用如下方式生成：

```bash
./ventus-env/install/bin/llvm-objdump -d --mattr=+v,+zfinx path/to/kernel.riscv > path/to/kernel.dump
```

## 验证内容

`verify` 会对每个函数做如下检查：

- 基本块与 CFG：
  - 直接分支/跳转目标必须落在函数内并成为基本块入口（leader）。
  - 块末尾若无显式控制转移，自动补一条 fallthrough 边到下一块。
- `setrpc/vbranch/join`：
  - 识别典型 `auipc <r>, imm` + `setrpc ..., <r>, off` 计算 join PC。
  - 每条 `vbranch` 必须能解析到一个 join PC，且该 PC 处存在 `join` 指令。
  - join 块需后支配对应 `vbranch` 所在块（post-dominator）。
  - 近似 SESE：从 `vbranch` 的两条后继出发到 join 的区域内不允许“侧出口”；区域应为单入口（只允许从该 `vbranch` 进入）。

若遇到原型阶段不支持的控制流（例如非 `ret` 形式的 `jalr`、无法解析 join PC 等），会在报告中标记为失败项，便于后续收敛输入约束或补齐分析能力。
