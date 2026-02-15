## 0. Scope lock (prototype contract)
- [x] 0.1 记录 Rodinia 输入清单与 kernel 入口符号（以 `.symtab` 为准；见 `design.md`）
- [x] 0.2 明确原型期输入约束与失败策略（见 `design.md` 的 Prototype contract）
- [x] 0.3 冻结关键设计选择（SIMT 结构化/地址空间方案/标量语义兜底；见 `design.md` 的 Key Decisions）
- [x] 0.4 定义每阶段验收标准与产物（见 `design.md` 的 Stage gates & artifacts）

## 1. Frontend: ELF → `.text` + decode (with `regext`)
- [x] 1.1 在本项目内提供 ELF 读取能力：提取 `.text` 与 `.symtab`（不做重定位）
- [x] 1.2a 建立 Ventus 指令匹配表来源（以 `ventus-env/spike/riscv/encoding.h` 为主）
- [ ] 1.2b 与 cyclesim decode table 做关键指令交叉验证（可选，后续补齐）
- [x] 1.3 实现 32-bit 指令解码与立即数字段抽取（覆盖 Rodinia 所需子集）
- [x] 1.4 实现 `regext` 前缀合并（严格“仅作用于下一条指令”）
- [x] 1.5 提供 pretty-printer/反汇编对照输出，用 `*.dump` 做 golden 校验

## 2. Midend: CFG build + `setrpc/vbranch/join` structural verification
- [x] 2.1 基于解码结果按 PC 切基本块并构建 CFG（显式边：branch/j/jal/ret/vbranch）
- [x] 2.2 实现 `setrpc` 的 join PC 静态求值（至少覆盖 Rodinia 形态：`auipc` + `setrpc`）
- [x] 2.3 对每条 `vbranch` 做结构化合法性校验：post-dominator / 单入口 / 无侧出口
- [x] 2.4 `barrier` 合法性检查：必须位于收敛点（无法证明则拒绝输入）
- [x] 2.5 输出可机器读取的验证报告（JSON），用于 CI/回归

## 3. Backend: Ventus → PTX bring-up (compile-first)
- [x] 3.1 定义 PTX 侧执行/状态模型：`WarpCtx` + active-lane leader，保证 warp-uniform 标量语义
- [x] 3.2 落地地址空间方案（原型期建议：方案 B，保留 Ventus 32-bit 数值地址并区间分流）
- [x] 3.3 实现 Rodinia 必需指令子集的 PTX lowering（覆盖控制流/访存/向量算术/必要浮点）
- [x] 3.4 生成 `.ptx` 并确保 `ptxas` 可编译（每个 kernel 产物可复现）
- [x] 3.5 定义“暂不支持”指令/形态的错误码与诊断信息（例如 `regexti`、kernel 内非标准 `jalr`）

## 4. (Optional) Toolchain integration: run via PoCL/driver
- [ ] 4.1 扩展 `ventus-env` driver 的 `ptx_device`：从 vecadd-only 变为按 `kernel_name` dispatch SBT 生成的 PTX
- [ ] 4.2 复用 PoCL 的 ELF 上传/metadata/arg buffer 路径，完成端到端运行
- [ ] 4.3 用 Rodinia 7 个测例建立回归脚本（先 bring-up 1~2 个，再扩展覆盖）
