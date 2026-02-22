# 改进点与通用化演进建议（不改代码版）

本文基于仓库当前实现（`sbt/` + `tools/`）与既有文档（`doc/archive/STATUS_SBT_PIPELINE_2026-02-19.md`、`doc/archive/HANDOFF_PHASE4_PTX_DEVICE_SBT_JIT.md`、`lab/03_sbt_feasibility/*`）整理：**只指出问题与给出改进方案**，不直接修改代码。

## 0. 你现在“已经拥有的东西”（利好）

- 端到端链路已经真实跑通（PoCL/driver + SBT PTX JIT），并且把关键坑（如 vbranch 操作数顺序）固化为实现与交接文档。
- Stage2（CFG verify）已实现一套“可解释的 fail-fast”机制：能定位 `vbranch/join/barrier/jalr` 的不满足条件点。
- emitter 已覆盖一批 Rodinia bring-up 所需指令，并把 warp-uniform 标量语义落地为可运行方案（per-warp shared `WarpCtx`）。

这些意味着：后续的“通用化”不需要推倒重来，核心是把 **“样例驱动的硬编码/默认假设”** 收敛为 **可配置、可验证、可扩展** 的工程结构。

---

## 1. 当前实现中偏“测例驱动”的地方（建议优先抽离）

### 1.1 指令白名单与编码表来源重复且分散

现状：
- bring-up whitelist 在至少三处重复：
  - `tools/sbt_ptx.cpp`（`want` 列表）
  - `tools/sbt_decode.cpp`（`want` 集合）
  - `tools/gen_spike_encoding_subset.cpp`（另一个 `want` 列表，且内容与前两者并不完全一致）
- runtime 依赖 `ventus-env/spike/riscv/encoding.h` 的文本解析（`sbt/spike_encoding_parser.*`）。

问题：
- 白名单内容不一致会导致“decode 能过/ptx 不支持”或相反的隐性分叉。
- 对 `ventus-env` 的路径依赖让仓库很难独立复现（尤其对 CI/外部用户）。

建议：
- **单一事实源（single source of truth）**：
  1) 用 `gen_spike_encoding_subset` 生成 `sbt/generated/spike_encoding_subset.hpp`（或 JSON），作为“pattern 子集”的稳定输入；
  2) `sbt_decode/sbt_ptx` 都只引用生成产物，不再各自维护列表；
  3) 将“支持子集”显式版本化（例如在生成文件头写入 Spike commit/encoding.h mtime）。
- 若担心“生成文件提交进仓库”的维护成本：至少把 whitelist 从代码搬到 `doc/`/`data/` 的一个列表文件，让工具加载同一份配置。

### 1.2 builtin call 支持集合偏 Rodinia/PoCL 经验列表

现状：
- `sbt/ptx_emit.cpp` 里硬编码 `is_builtin_call_name()` + 一组内联实现（`get_global_id`/`sqrt`/`cos/sin/tan`/`mad24` 等）。

问题：
- 这是“跑通当前测例”的正确做法，但会把 emitter 变成“PoCL/OpenCL runtime 适配器”，难以扩展到更多 libc/clc/数学函数。
- builtin 的 ABI（参数在哪个寄存器/返回值在哪）目前隐含在实现里，缺少文档化与可测试性。

建议：
- 把 builtin 支持抽象成一个“小的可扩展层”：
  - 形式 A：`BuiltinRegistry`（name → lowering handler），并把 handler 作为独立文件/模块（避免 emitter 继续膨胀）。
  - 形式 B：把 builtin 视为“外部库”，优先尝试链接/调用 NVIDIA `libdevice` 或 PTX 内建（能用则用），不能用再内联兜底。
- 为每个 builtin 建立 micro-test（不必依赖 Rodinia），输出可比对结果，避免“某个 benchmark 变了才发现 builtin 实现错了”。

### 1.3 （已修正）`s0` bump：不要在翻译器 prologue 中做“补偿猜测”

现状（修正后）：
- `sbt/ptx_emit.cpp` 的 prologue 仅按 `_start` ABI 初始化 `x8(s0)=CSR_LDS+CSR_NUMW*1024`；kernel 自身若执行 `addi s0, s0, imm`，视为 frame 分配，不再由翻译器做“抵消补偿”。

问题：
- 这是一个典型“测例驱动的 ABI hack”：它依赖 PoCL 当前生成代码形态；换编译器版本/优化等级/别的运行时可能就失效。

补充说明：为什么之前的 `detect_s0_bump_bytes()` 属于 PoCL 特定 prologue 模式匹配、具体风险是什么？

- **它在做什么**  
  旧实现会在 kernel 的前若干条指令里扫描一种“固定形态”的常量加法序列，来推断 `bump_bytes`，典型有两类：
  - 大常量（frame 较大时常见）：
    - `lui t0, hi(frame)`
    - `addi t0, t0, lo(frame)`
    - `add s0, s0, t0`
  - 小常量（<= 2047）：
    - `addi s0, s0, imm`

  然后翻译器在 PTX prologue 里**反向补偿**：把初始化的 `s0` 改成 `s0_init = (LDS_base - bump_bytes)`，让 kernel 执行完 bump 后“回到” `LDS_base`。

- **它依赖的隐含假设（PoCL/特定 LLVM 代码形态）**  
  这套做法隐含了很多对“编译器生成 prologue 风格”的假设，例如：
  - bump 一定出现在函数最开头的很小窗口内（旧实现只看前 ~12 条指令）；
  - bump 一定是“为了把 `s0` 调到 LDS base”，而不是“为了给 frame/局部数组/溢出/spill 留空间”；
  - bump 由 `s0` 自身加常量构成，且常量构造恰好是 `lui/addi/add` 或单条 `addi`；
  - 参与计算的临时寄存器固定且不被其它指令扰动（例如用 `t0` 但不被 clobber）；
  - kernel 后续对 LDS 的寻址风格与 bump 语义一致（例如“正偏移访问”）。

  这些都不是 ISA/ABI 的硬约束，更像是 PoCL + 某个 LLVM 版本/优化等级在当前测例上的“经验形状”。

- **具体会错在哪（为什么会阻碍通用化）**  
  最关键的错误风险是：**把“frame 分配”误判成“LDS base 对齐”**。
  - 很多编译器会把 `s0` 当作 frame pointer / stack-like pointer 使用：先 `s0 += frame_bytes`，随后用 `s0 - k`（负偏移）访问 frame 内对象。  
    这时如果翻译器做了 `s0_init = LDS_base - frame_bytes`，那么 bump 后 `s0 = LDS_base`，负偏移会访问到 `LDS_base` 以下的地址，等价于把整个 frame 平移到一个错误区域，轻则数据错，重则越界。
  - 相反，如果 kernel 真的按“正偏移访问”写法使用 `s0 + k`，那么 `s0_init = LDS_base - bump` 的补偿才可能“看起来”正确。问题在于：你无法从少量模式匹配可靠地区分这两类语义。

  本质上，`detect_s0_bump_bytes()` 在翻译器里试图“推测 runtime/ABI 应该如何选择 CSR_LDS”，这是通用化的大坑：ABI 应该由 runtime/driver 决定并可验证，而不是由翻译器从测例形状倒推。

建议（通用化方向仍然成立）：
- 把“kernel ABI 适配”显式分层（而不是掺在 emitter 里）：
  - `AbiAdapter::init_regs()`（或类似概念）负责 x2/x8/x10/CSR_KNL 等初始化策略；
  - 对需要显式传入的 ABI 参数（例如 LDS base、arg base、资源信息），优先让 runtime/driver 提供，而不是在翻译器里靠模式匹配“猜”。

### 1.4 `testcases/simple` 的脚本不可复现且语义假设过旧

现状：
- `testcases/simple/compile.sh` 含用户绝对路径/`~`；`simple.S` 直接用 `0x9000_0000` 作为基址。

问题：
- 这会让“最小测试”难以在新机器/CI 上复现。
- 固定地址假设与当前端到端 driver 的 backing 模型不完全一致，容易误导后续读者。

建议：
- 明确定位：`testcases/simple` 是“历史原型/语义参考”还是“持续可跑的单元测试”。
  - 若要持续可跑：把脚本改成只依赖 `./ventus-env/install/bin` 相对路径，并把基址作为参数/CSR 传入（与当前 Stage3/Stage4 约定一致）。
  - 若仅作参考：在目录 README 里标注“不可复现脚本/旧假设”，避免把它当成标准 pipeline。

---

## 2. 向“通用化”演进：建议的技术方向（按收益/风险排序）

### 2.1 把“支持范围”变成可查询的能力矩阵（Capability/Feature）

现状：
- 支持范围散落在代码与文档里：哪些指令/哪些控制流/哪些 builtin 能跑，主要靠经验与 fail-fast 报错。

建议：
- 定义一个“能力矩阵”作为公共 API：
  - `supported_insts`（按指令名）
  - `supported_cf_forms`（ret-only jalr / 可结构化 vbranch / barrier 规则等）
  - `supported_builtins`
  - `address_model`（方案 A/B/C）
  - `abi_adapter`（pocl/standalone/…）
- 工具输出中加入“输入 kernel 需要哪些 feature”（可由静态扫描/CFG verify 推导），从而：
  - 更快定位 bring-up 缺口；
  - 在扩展时能做到“增量支持，回归可控”。

### 2.2 引入显式 IR 层（解码 IR vs 语义 IR vs 后端 IR）

现状：
- `DecodedInst` 同时承担：解码结果 + 一部分语义分类（suffix 推断）+ emitter lowering 的输入。

问题：
- 随着指令覆盖增长，这种“字符串 name + 若干字段”的结构会让：
  - verify/emit 的匹配分支急速膨胀；
  - 很难做统一优化与一致性检查（尤其是地址/类型/宽度）。

建议：
- 分两层最小 IR 即可，不必一步到位：
  1) **Decode IR**：保持当前 `DecodedInst` 形态，但把“按 suffix 推断 operand”的规则集中化/表驱动；
  2) **Semantic IR（可选）**：把常见操作归一（例如 `BinOp{Add/Sub/And/...}`、`Load/Store`、`Branch`、`BuiltinCall`），并明确每个 op 的类型与空间（u32/u64、shared/global/local）。
- 好处：后端（PTX）和未来可能的其它后端/解释器可以共享同一份语义层，不再在 emitter 里硬编码所有规则。

### 2.3 统一并“可选化”地址模型（A/B/C），减少 ABI 假设耦合

现状：
- 现实现更接近地址空间方案 B（保留 Ventus 数值地址、按区间显式分流）。
- `lab/03_sbt_feasibility/address_space.md` 对方案 A/B/C 有系统分析，但代码层没有清晰的模式开关与一致性检查。

建议：
- 把地址模型做成显式 option（并写进输出 PTX 注释/metadata）：
  - `numeric-range-split`（当前方案）
  - `generic-pointer-rewrite`（若未来要走方案 A）
  - `hybrid`（方案 C）
- 同时把 driver/ABI 依赖点集中化：避免 emitter/driver/文档对同一地址区间与布局各写一份。

### 2.4 把“CFG verify”与“lowering 约束”对齐成可用的诊断规范

现状：
- verify 输出已有结构（vbranch/barrier/jalr），但 emitter 的 unsupported/error code 与 verify 的 reason 尚未完全统一。

建议：
- 统一诊断码与字段（至少在 CLI JSON 输出中固定）：
  - `reason_code`（machine-readable）
  - `pc`（u32 hex）
  - `inst_name`/`callee`/`csr`
  - `detail`（human-readable）
- 这样可以让“回归系统/测例系统”按 reason 聚类，形成真实的 bring-up backlog。

---

## 3. 面向测例体系的改进建议（让回归更像“工程”而不是“脚本集合”）

### 3.1 用数据驱动描述 testcases（替代硬编码列表）

现状：
- `tools/rodinia_ptx_smoke.sh`：kernel 列表硬编码。
- `tools/ventus_regression_profile.py`：testcase 列表硬编码。

建议：
- 在仓库新增一个 `testcases/manifest.json`（或 YAML）：
  - 每项包含：输入 ELF、kernel 名、是否 require_known、预期行为（ptxas 编译/端到端运行）、timeout、参考输出位置等。
  - `rodinia_ptx_smoke` 与 profile 脚本都读取同一份 manifest。
- 额外收益：未来可以自然支持“新增 benchmark 只改数据，不改脚本/代码”。

### 3.2 把“compile-first”与“run”分成可组合 pipeline

建议把回归拆成两个独立层级：
- Layer1：`ELF → PTX → ptxas`（无需 GPU 运行即可做，最适合 CI）
- Layer2：`PoCL/driver → JIT → run`（依赖 GPU/权限/运行环境）

对每个 testcase 显式标注属于哪一层，并把失败原因写入统一 JSON（用于统计与趋势分析）。

### 3.3 为“非功能失败”建立清晰分类（环境/权限/工具链）

端到端 bring-up 时常见失败并非翻译错误，例如：
- `cuInit` 权限问题（沙箱/容器 `/dev/nvidia*` 权限）
- `ptxas`/CUDA 版本与 `.version/.target` 兼容问题
- `ventus-env` 中某些脚本/产物缺失导致运行失败

建议：
- 回归脚本将失败分成：`ENV_ERROR` / `TOOLCHAIN_ERROR` / `TRANSLATION_ERROR` / `RUNTIME_MISMATCH`；
- 并把“环境检查”做成独立命令（可在 CI 前置 fail-fast）。

---

## 4. 文档与规格（让“通用化”有可审查的落地路径）

现状：
- `openspec/changes/*` 有阶段方案与 requirement，但 `openspec/specs/` 为空；已落地能力与未来计划之间缺少“当前真相（IS built）”的沉淀层。

建议：
- 把已落地能力沉淀为至少一个 capability spec（示例）：
  - `openspec/specs/sbt-ptx/spec.md`：描述当前支持的输入/约束/输出/诊断规范
  - `openspec/specs/sbt-ptx/design.md`：记录关键设计（WarpCtx、地址模型、verify 约束）
- 后续大的通用化改动（例如引入 IR 层、切换地址模型、抽离 ABI adapter）再以 OpenSpec change 的形式推进，避免“文档/实现分叉”。

---

## 5. 一个可执行的“演进路线”（建议的拆分方式）

下面按“能独立验收、且尽量不引入破坏性”的顺序排列（每条都建议走 OpenSpec change 做 gate）：

1) **统一指令白名单与 pattern 源**（生成文件/配置文件化；移除重复列表）
2) **测试清单 manifest 化**（compile-first 与 run 分层；失败分类标准化）
3) **builtin 支持层模块化**（registry + micro-tests）
4) **ABI adapter 分层**（把 PoCL 专有 prologue 规则从 emitter 主体抽离）
5) **诊断规范统一**（verify 与 emitter 的 reason_code 对齐；JSON 结构固定）
6) **（可选）引入语义 IR**（以最小集合起步，逐步迁移 emitter）
7) **地址模型显式化**（A/B/C 可选 + 一致性约束；driver/emit 同源配置）
