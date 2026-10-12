# PTX 冗余指令优化工作日志

## 任务预期目标

- 全面审查当前 SBT PTX 生成路径，确认并去除可证明无用的指令与临时寄存器。
- 用户示例的三条指令应缩为 `ex2.approx.f32 %v6, %v6;`。
- 保持寄存器覆盖、谓词、分支、内存副作用、同步与 ABI 语义，失败显式暴露。
- 补充有针对性的自动化验证，同步当前实现文档，必须通过 `tools/regress.sh --preset all --arch sm_89` 全量回归。

## 当前实施状态

已完成：清理实现、28 个边界测试、当前文档和回归入口同步均完成；最终全量回归退出码为 0，输出 `ALL OK (preset=all)`。

## 最终交付成果

- 代码：`sbt/ptx_cleanup.{hpp,cpp}` 与 function assembly/寄存器声明接入；删除 entry 和 scalar FCLASS 的明确死代码。
- 验证：28 个清理边界测试；统一入口包含 13 个 C++ 单测、mnemonic allowlist、MMA/non-MMA、Rodinia/PDS、普通微测例、端到端。
- 回归修复：feature probe 改为检查当前 repository-local Spike 实现；补充输出可见、真实执行的双向 `bltu` 覆盖，并在失败时打印缺失 mnemonic。
- 文档：`README.md`、`doc/IMPLEMENTATION_CODEMAP.md`、`doc/README.md`、`doc/PTX_REDUNDANCY_CLEANUP.md`、`tools/README.md` 已同步。
- 11 个 Rodinia kernel 的 PTX 总指令减少 6.6%，mov 减少 13.3%；未宣称 SASS 或运行时间收益。
- 最终日志：`build/ptx-cleanup-full-regression.log`；端到端 JSON：`build/ptx-cleanup/full-regression/build/ventus-regression-profile/summary.json`；前后统计：`build/ptx-cleanup/comparison/summary.json`。
- 所有源代码变更位于 `sbtsim`；未覆盖 `../install`，未提交。已恢复全量测试构建改变的其它子仓库 tracked 二进制。

## 即时工作日志

### 2026-10-11 15:39 UTC

- 设置 goal，创建本工作日志。修改范围以 `sbtsim` 子仓库为主；现有 `ocl-icd`、`systemc` 工作区状态与本任务无关。
- 已读取上级和子仓库 `AGENTS.md` / `README.md`，定位 `sbt/ptx_emit_*.cpp`、`CMakeLists.txt` 与统一回归入口。
- NVIDIA PTX 官方 operand type checking 规则确认：bit-size 类型兼容同宽类型；`.b32` 的 `%v` / `%x` 可以直接供 `.f32` 指令使用，因此示例两端 `mov.b32` 不必存在。
- 同类拷贝出现在 vector/scalar FP、custom SFU/conversion、builtin、标量整数和地址映射；另发现零立即数整数操作、自拷贝、跳到紧邻标签、未使用临时寄存器等可审查类别。
- 环境具有 `ptxas` 与 RTX 4090，可运行真实 compile-first / Spike-vs-PTX / 端到端 gate。回归默认绑定当前工作树 `build/sbt_ptx`，无需覆盖 `../install`。
- 优化正确性重点：不可跨寄存器重定义传播旧值，不可忽略谓词执行差异，不可删除内存访问/同步/原子/warp collective 的副作用；浮点加零、乘一也不能按整数恒等式处理。

### 2026-10-11 16:03 UTC

- 新增函数级 `sbt/ptx_cleanup.{hpp,cpp}`：同宽 copy propagation、相邻 producer/writeback 合并、局部死 scratch 消除、整数恒等操作、自拷贝与相邻标签跳转删除；只声明仍被使用的 temp / fixed register group。
- 删除 entry 中立即被覆盖的 legacy shared scalar backing 地址计算和提前的 stack-size metadata 读取；删除 scalar FCLASS 中六条无读取方的旧固定谓词计算。
- 清理不将 `.f32` scratch 直接传播到整数操作。内存访问、trap、同步、原子与 collective 保留。保守保留跨段引用和段内先读后定义的 scratch，避免误删循环携带值；guard-aware definition facts 在谓词重定义时失效。
- 测试曾发现同谓词写回被过度保留，已修复分析；28 个清理测试通过。旧 ABI 断言同步改为检查直接读取 `%x` 的计算与显式 fallthrough，不要求保留冗余 `mov` / `bra`。
- 将统一回归原先遗漏的 metadata、builtin、external mnemonic、call prototype、replicated scalar-state 测试加入 quick/all；C++ 单测统一施加 180 秒硬超时。
- custom emit（含用户 in-place EX2 示例）、MMA emit、PDS、broadcast、metadata、call prototype、ABI 测试通过；B+tree/backprop/NN/kmeans 四个准备阶段端到端用例通过。
- 11 个 Rodinia kernel 对比：PTX 指令 11051 → 10321（减少 730，约 6.6%），mov 2208 → 1914（减少 294，约 13.3%），virtual temp 声明 3882 → 3762。明细：`build/ptx-cleanup/comparison/summary.json`。不宣称相应 SASS/运行时间收益。
- 全量普通 microtest 的 18 个 Spike/PTX buffer compare 全部通过，但 coverage 为 245/246；增加缺口诊断输出后定位到 `bltu`。不降低 gate 阈值、不新增 exceptions，准备补真实执行覆盖。
- feature probe 仍检查旧 `dependencies/mma-sim` / `dependencies/unfu`，误跳过当前已在 `riscv/ventus_mma.h` / `ventus_custom_arith.h` / `ventus_custom.cc` 实现的 MMA/SFU。修正为当前实现文件检查后五类 feature 均 available，下一轮将真实执行全部 custom gate。

### 2026-10-11 16:09 UTC

- 在 `mt_rv32i_branch_cov` 中增加 `bltu a,b` 与 `bltu b,a`，各分支结果参与输出 hash；不依赖其它应用偶然生成 BLTU，未调整 exceptions 或 coverage 阈值。
- 最终命令（在 `sbtsim/` 下，已先运行 `cmake --build build -j 8`）：

  ```bash
  source ../env.sh
  tools/regress.sh --preset all --arch sm_89 --no-build \
    --workdir /work/ventus-env/sbtsim/build/ptx-cleanup/full-regression \
    --keep-workdir > build/ptx-cleanup-full-regression.log 2>&1
  ```

- 最终退出码 `0`，日志末尾 `ALL OK (preset=all)`。
- 13 个 C++ 单测和 emitter mnemonic allowlist 全部通过；清理边界测试 28/28。
- MMA：8/8 supported family compile-first；两组规模共 16/16 三方语义比较通过。另一个 non-current row.row case 按原合同通过预期的 `unsupported.mma.deferred` 拒绝测试，其 semantic skip 是该负例的既定行为。
- non-MMA：36/36，`skipped=0 failures=0`；SFU 和 MMA 没有因 feature probe 跳过。
- Rodinia：11 个 compile-first kernel 全部通过；PDS smoke 通过。
- 普通微测例：18/18 Spike/PTX compare；coverage 为 `covered_unique=246 uncovered_unique=0 ratio=1.000`，唯一原有 exception 仍为 `jalr`。
- 端到端：15/15，包括 matadd/vecadd、Rodinia、MNIST/AlexNet/LeNet/ResNet；均实际使用当前工作树 `build/sbt_ptx`。
- `git diff --check`、Python py_compile 和 bash 语法检查通过；新清理实现 448 行，各函数少于 100 非空行。最终复核源码未再改变已测试行为。
- 清理其它子仓库由测试重建产生的 5 个 tracked `.out` 二进制；原先的 `ocl-icd` / `systemc` 工作区状态未改动。
