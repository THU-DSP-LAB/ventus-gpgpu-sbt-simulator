# lab/04_instruction_decode：Ventus 指令解码（为 SBT 前端铺路）

静态二进制翻译（SBT）首先要能从 Ventus ELF 中稳定地“读出正确的指令与操作数”。本实验聚焦**指令解码**本身，并基于 `ventus-env` 里两个现有仿真器（`cyclesim` 与 `spike`）的实现，给出本项目 SBT 前端的初步流程计划。

> 语义基准：若 `VentusInst_basic.txt` 与 `doc/ventus-isa/*` 有冲突，以 `doc/` 为准。

## 1. 现有实现调研：cyclesim（周期精度仿真器）

cyclesim 的解码逻辑来自硬件 Chisel 工程（`DecodeUnit.scala` / `Instructions.scala`），并通过脚本生成 C++ 侧的表格。

### 1.1 指令匹配（match/mask → opcode）

- Chisel BitPat 定义：`ventus-env/cyclesim/src/sm/Instructions.scala`
- 生成脚本：`ventus-env/cyclesim/src/sm/init_instable.py`
- 生成结果：`ventus-env/cyclesim/src/sm/init_instable.cpp`
  - 结构是按 `mask` 分组的 `unordered_map<masked_insn, OP_TYPE>`，与 Spike `MATCH_* / MASK_*` 一致。
  - 例如（从生成结果可直接看到）：
    - `JOIN_`：`masked=0x205b`、`mask=0x707f`
    - `SETRPC_`：`masked=0x305b`、`mask=0x707f`
    - `REGEXT_`：`masked=0x200b`、`mask=0x707f`
    - `VBEQ_`：`masked=0x005b`、`mask=0x707f`

### 1.2 解码字段（操作数/立即数/执行单元等）

- Chisel decode 表：`ventus-env/cyclesim/src/sm/DecodeUnit.scala`
- 生成脚本：`ventus-env/cyclesim/src/sm/decodetable_init.py`
- 生成结果：
  - `ventus-env/cyclesim/src/sm/init_decodetable.cpp`：`OP_TYPE -> decodedat`
  - `ventus-env/cyclesim/src/sm/decodetable.md`：同内容的 markdown 表格（便于人工查看）

### 1.3 decode 主流程（含 regext/regexti 前缀）

核心实现：`ventus-env/cyclesim/src/sm/decode.cpp`

- **step1**：用 `init_instable.cpp` 生成的 `mask + itable` 找到 `OP_TYPE`
- **step2**：提取 `rd/rs1/rs2/rs3/imm` 等字段，并用 `decodedat.sel_imm` 选择立即数格式（I/S/B/U/J/Z/V/L11/S11 等）
- **前缀**：`REGEXT_ / REGEXTI_` 在 decode 阶段“只记录扩展信息，不下发到后级执行”，并且只作用于**下一条**实际指令
  - `REGEXT`：扩展 `rd/rs1/rs2/rs3`
  - `REGEXTI`：扩展 `rd/rs2`，以及 `vop.vi` 的立即数高位（`extimm << 5`）

### 1.4 SIMT 控制流语义参考（vbranch/join）

虽然本 lab 的重点是“解码”，但 SBT 一旦处理 `vbeq/vbne/... + setrpc + join` 就不可避免涉及 SIMT 语义。cyclesim 的硬件风格实现可作参考：

- `ventus-env/cyclesim/src/sm/exec_simtstk.cpp`
  - 以 `CSR_RPC (0x80c)` 作为收敛点
  - 分支时根据 `if_mask/else_mask` 的线程数选择先执行哪条路径，并压栈保存另一条路径与收敛信息
  - `join` 在 `PC == rpc` 时弹栈并跳转到 `nextpc`，同时恢复 `nextmask`

## 2. 现有实现调研：spike（功能仿真器，关注 Ventus 相关扩展）

spike 的优势是：它的 `match/mask`、反汇编格式、以及部分 Ventus 自定义指令语义都非常“直给”，便于作为 SBT 前端的参考实现。

### 2.1 Ventus 自定义指令的编码真值表（建议作为主来源）

位置：`ventus-env/spike/riscv/encoding.h`

这里显式给出了 Ventus 相关扩展的 `MATCH_* / MASK_*`，例如：

- `MATCH_JOIN 0x205b` / `MASK_JOIN 0x707f`
- `MATCH_SETRPC 0x305b` / `MASK_SETRPC 0x707f`
- `MATCH_REGEXT 0x200b` / `MASK_REGEXT 0x707f`
- `MATCH_VBEQ 0x005b` / `MASK_VBEQ 0x707f`
- `MATCH_ENDPRG 0x400b` / `MASK_ENDPRG 0xfe00707f`
- `MATCH_BARRIER 0x400400b` / `MASK_BARRIER 0xfe00707f`

同时，`ventus-env/spike/riscv/riscv.mk.in` 的 `riscv_insn_ext_v_gpgpu` 列表可用作“Ventus 扩展指令清单”。

### 2.2 反汇编（用于做 decode 结果对照）

位置：`ventus-env/spike/disasm/disasm.cc`

可以看到 Ventus 扩展指令走哪些格式化宏（例如 `DEFINE_VBRANCH_TYPE(vbeq)`、`DEFINE_REGEXT_TYPE(regext)` 等），这对我们做一个最小 disassembler/pretty-printer 很有帮助。

### 2.3 regext/regexti 的前缀语义（非常关键）

spike 用“执行 regext 指令 + 保留扩展状态一条指令”的方式实现前缀：

- 指令语义入口：
  - `ventus-env/spike/riscv/insns/regext.h`
  - `ventus-env/spike/riscv/insns/regexti.h`
- 扩展字段编码规则（建议直接复用这套规则）：
  - `ventus-env/spike/riscv/processor.h`：`processor_t::ext_set / ext_rs* / ext_imm / ext_valid`
- “只作用于下一条指令”的生命周期控制：
  - `ventus-env/spike/riscv/execute.cc`：执行完一条指令后，若该条是 regext，则仅清 `regext_enable`；否则清空扩展信息

另外，spike 在 `ventus-env/spike/riscv/decode.h` 里把 `ext_rs1/ext_rs2/ext_rs3/ext_rd` 直接 OR 到寄存器索引上；在 `ventus-env/spike/riscv/v_ext_macros.h` 里把 `regexti` 的 `ext_imm` 只用于扩展 `vop.vi` 的 5-bit 立即数（而不是 I-type 的 12-bit 立即数）。

### 2.4 SIMT 控制流语义参考（vbranch/join/setrpc）

关键位置：

- `vbranch`：`ventus-env/spike/riscv/insns/vbeq.h` 等 + `ventus-env/spike/riscv/v_ext_macros.h` 的 `VV_LOOP_BRANCH`
  - 分支计算 per-lane 条件 → 形成 `if_mask/else_mask`
  - `push_branch(CSR_RPC, if_pc, if_mask, r_mask, else_pc, else_mask)`
  - 跳到下一条要执行的路径，并更新当前 mask
- `join`：`ventus-env/spike/riscv/insns/join.h`
- `setrpc`：`ventus-env/spike/riscv/insns/setrpc.h`

## 3. 对本项目原型的直接启发：如何复用

### 3.1 编码表：以 spike `encoding.h` 为主、cyclesim 交叉验证

原因：

- spike 的 `MATCH/MASK` 明确、集中、易于自动提取；
- cyclesim 的 `init_instable.cpp` 是由硬件 BitPat 生成，可用作交叉验证（两者在 Ventus 扩展上应一致）。

建议策略：

1. 从 `ventus-env/spike/riscv/encoding.h` 自动提取 `MATCH_* / MASK_*`（只取 `VentusInst_basic.txt` 中需要关注的指令子集）
2. 生成本项目自己的 `decode_table`（`mask -> {match -> op}` 或 `op -> {mask, match}`）
3. 用 cyclesim 生成表做一致性检查（至少覆盖：`vbranch/join/setrpc/regext/endprg/barrier/vls12/vadd12.vi`）

### 3.2 regext

SBT 的 decode 需要“把前缀与下一条指令合并”为一个逻辑指令（否则寄存器编号/立即数会错）。

推荐直接以 spike 的规则为准：

- `regext`：扩展 `rd/rs1/rs2/rs3`
- `regexti`：扩展 `rd/rs2` 与 `vop.vi` 立即数高位
- 作用范围：只影响下一条指令，然后清除

当前可只实现 `regext`，遇到 `regexti` 直接报错即可

### 3.3 立即数抽取：优先借鉴 cyclesim 的实现

cyclesim 的 `decode.cpp` 已把多种立即数格式（I/S/B/U/J/Z/V/L11/S11）以 C++ 的方式写出来，且与其硬件 decode 表一致；这对我们实现一个“无歧义的字段提取器”很有帮助。

## 4. 本项目 SBT 全流程（草案）

结合现有实验（`lab/00~03`）与上述解码实现，SBT 的整体流程可按“前端→中端→后端”拆开：

1. **输入准备**
   - 输入：PoCL/Ventus 工具链产物 `object0.riscv`（RISC-V ELF）+ metadata buffer + arg buffer
   - 运行时入口：沿用 `ventus-env/driver` 的加载与 host-device ABI 约定（见 `lab/01`、`lab/02`）

2. **ELF 装载与地址视图建立**
   - 解析 PT_LOAD 段，建立“Ventus 数值地址 → 实际承载”的映射
   - 识别 `.text` 与 kernel entry（以及必要的静态数据/重定位信息）

3. **指令流解码（本 lab 的核心）**
   - 在 PC 维度解码为 `DecodedInst`：`{pc, len, op, rd/rs*, imm, flags...}`
   - 处理 `regext/regexti` 前缀并与下一条指令合并
   - 不把“可能是垃圾数据的字节”当作有效指令：应由 CFG 驱动扫描而非盲目线性扫完整个段

4. **CFG 构建 + 静态求值**
   - **CFG 目标**：把“可达的指令流”从 ELF/`.text` 中**抽取成基本块图**，同时为后续翻译提供：
     - 每条指令的唯一定位（`pc -> inst_id/label`）
     - 每个基本块的出边（fallthrough / branch / call / return / join）
     - 一组“必须静态已知”的关键值（例如 `setrpc` 的 reconvergence PC）
   - **基本块切分规则（建议）**
     - leader：
       - entry（kernel entry、函数 entry）
       - 所有已知跳转目标（`beq/bne/.../jal`、`vbeq/vbne/...` 的目标与 fallthrough）
     - terminator：
       - 任何改变 PC 的指令（标量分支/跳转、`vbranch`、`join`、`endprg`）
       - `barrier` 不改变 PC，但对“不能在发散路径执行”的约束很敏感：建议在 IR 上也当作“控制流敏感点”记录下来，供后续校验/改写
   - **边类型（建议至少区分）**
     - `fallthrough`：`pc+4`（或 RVC 时 `pc+2`，若暂不支持 RVC 可先不做）
     - `branch_taken`：直接分支目标（B-type、`vbranch`）
     - `call` / `ret`：来自 `jal/jalr`（可结合 `lab/03_sbt_feasibility/function_call.md` 的约束/方案）
     - `join_edge`：`join` 的“可能跳转到 else_pc / nextpc”的语义边（需要结合 `setrpc`/SIMT 语义推导）
   - **函数边界/入口识别（可选但很有价值）**
     - 优先读 ELF 的 `symtab`/`dynsym`（若存在）来获得函数符号与入口；
     - 若缺符号：以 kernel entry 为根做可达性扫描即可（原型阶段不必完整恢复所有函数）。
   - **静态求值（必须做，且与解码强耦合）**
     - 目标 1：静态求出 `setrpc` 写入的 `CSR_RPC`（reconvergence PC）
       - 依据：`setrpc rd, rs1, imm` 的语义是 `rpc = rs1 + imm`（spike），而 `rs1` 往往来自 `auipc`/PC-relative 计算；
       - 做法：对 **标量寄存器（warp-uniform）** 做常量传播/表达式求值（可从“只跟踪 `PC + 常量`”的轻量抽象开始）。
     - 目标 2：尽可能静态化 PC-relative 地址（常量池、静态数据、跳转表）
       - 原型可只支持常见模式（如 `auipc + addi`、`auipc + lw/sw`），遇到复杂重定位/间接跳转则拒绝或降级。
     - 目标 3：为后端选择策略提供信息
       - 能否结构化控制流（是否满足 `simtstack_hardware.md` 的输入约束）
       - 是否存在不可处理的 `jalr`/间接跳转
   - **是否复用开源工具来“重建 CFG”？**
     - 现成开源框架确实存在（例如 Ghidra、angr、BAP、radare2），但它们要么依赖**完整可用的反汇编/指令语义**，要么需要为自定义指令补齐 decoder/lifter。
     - Ventus 有一批自定义指令（`vbranch/join/setrpc/regext...`），通用工具默认并不认识；要让它们可用，通常需要：
       - 为 Ghidra 写/改 SLEIGH 规格；或
       - 为 angr/BAP 写 lifter/semantics；或
       - 让 disassembler（Capstone/LLVM MC 等）支持自定义编码。
     - 另外也存在“二进制 lifting 到通用 IR 再分析”的开源路线（例如 revng/retdec/Remill/McSema 等生态），但同样绕不开“自定义指令的 decoder/语义补齐”，工程量通常不比自建 CFG 小。
     - 结论（原型阶段）：**CFG 算法可借鉴，但实现上建议自建 CFG builder**，直接基于我们从 spike/cyclesim 复用的 decode 结果来建图；并把开源工具更多当作“人工调试/交叉验证”的辅助手段。

5. **Ventus → PTX 翻译**
   - **是否需要 IR？（建议：需要一个“够用的”中间表示）**
     - 不一定要上 LLVM SSA 那么重，但建议至少有两层：
       - `DecodeIR`：紧贴 `DecodedInst`（便于对照 dump/commitlog）
       - `CFG/BlockIR`：基本块 + 显式 terminator + 显式边 + 规范化操作数（便于做静态求值、重写控制流、插入 mask/地址空间路由逻辑）
     - 理由：直接“边解码边吐 PTX 字符串”会把控制流重写、mask 插桩、地址空间路由、uniform 处理等逻辑耦死在一起，后期会很难扩展/定位问题。
     - **可选路线：lift 到 LLVM IR 再走 NVPTX 后端**
       - 优点：可复用 LLVM 的分析/优化、寄存器分配与 NVPTX 代码生成能力；
       - 风险：二进制级语义（尤其是 `vbranch/join/setrpc`、地址空间路由、以及标量 warp-uniform 语义）很难“自然地”落在通用 LLVM IR 上，往往需要大量自定义 intrinsic/约束，并且还要防止后端优化破坏语义；
       - 结论：**可作为中长期方向或局部加速**，但原型阶段建议先以“自定义 IR + 直接 PTX emitter”为主线更可控。
   - **PTX 生成的两种大方向（与 `lab/03` 的 SIMT 方案对齐）**
     1) **偏“硬件分歧/收敛”的翻译（优先）**
        - 目标：把 `vbranch + setrpc + join` 还原成 PTX 可表达的分支结构，让 NVIDIA 硬件收敛机制承担大部分工作；
        - 需要：CFG + `setrpc` 静态化 + 满足 `simtstack_hardware.md` 的输入约束；
        - 关键难点：Ventus 标量（warp-uniform）在 PTX 上的落地，必须避免“发散路径只更新了部分 lane 的标量寄存器”破坏后续路径（参考 `lab/03_sbt_feasibility/scalar_uniform_ptx.md`）。
     2) **偏“软件 SIMT 栈解释器”的翻译（兜底）**
        - 目标：在 PTX 里维护 `pc/mask/stack`，按 `pc` 做 dispatch（类似解释执行），所有有副作用的指令都在 `@p_act`（virtual mask）下执行；
        - 优点：对控制流结构要求更弱，更多依赖“指令语义的机械翻译”；
        - 代价：PTX 代码体积与运行开销更大，但作为原型/兜底路线很实用（参考 `lab/03_sbt_feasibility/simtstack_software.md`）。
   - **寄存器/数据模型映射（必须明确）**
     - `v*`（向量寄存器）：每个 lane 一份，在 NVIDIA 上天然对应“每线程寄存器”（每个 thread 持有本 lane 的元素）。
     - `x*`（标量寄存器，Ventus 语义是 warp-uniform）：在 NVIDIA 上默认也是“每线程寄存器”，但语义不同：
       - 必须保证任意时刻各 lane 看到的 `x*` 一致；
       - 需要按 `scalar_uniform_ptx.md` 的策略处理（例如：统一由某个 lane 计算并广播 / 通过 shared 持久化 / 或通过控制流改写避免不一致）。
     - 类型系统：原型阶段可先按 `u32/s32/f32` 覆盖 `VentusInst_basic.txt` 里需要的类型，逐步扩展到 `u64/f64` 等。
   - **访存与地址空间路由（系统性问题，不是“某条 ld/st 翻哪条”）**
     - 统一数值地址 → PTX address space 的映射与路由见 `lab/03_sbt_feasibility/address_space.md`；
     - 翻译器需要在生成 PTX 时显式处理：
       - shared/global/（可能的）静态数据区的区分
       - 对齐、符号扩展、inactive lane 是否发出访存（与 mask 实现绑定）
   - **CSR/ABI 映射（与 driver 集成点）**
     - `CSR_TID/NUMT/WID/...`：能用 PTX builtin 的尽量用 builtin；其余通过 kernel param/metadata 指针传入（与 `lab/01`、`lab/02` 一致）
     - `CSR_KNL/LDS/PDS`：按既定 ABI 传入并参与地址计算
   - **函数调用/返回**
     - 若要支持函数调用：需要先确定 “Ventus call/ret 子集 + PTX 上的等价方案”（参考 `lab/03_sbt_feasibility/function_call.md`）
     - 原型阶段可先限制：不出现复杂 `jalr` 间接跳转，只支持常见 call/ret 模式
   - **PTX 输出形态**
     - 先输出可读的 `.ptx`（便于 diff/排错），再由现有 `ptx_device` 路径 JIT 生成 cubin 并执行（与 `lab/00`、`lab/02` 的运行闭环一致）。

6. **产物生成与执行**
   - 产物：PTX（或 cubin）+ wrapper kernel + 必要的常量/表
   - 运行：通过 `VENTUS_BACKEND=ptx` 走 `ptx_device` 在 NVIDIA GPU 上执行

7. **验证闭环**
   - 以 spike commitlog / objdump dump 做“golden 对照”（至少对比：`pc/op/imm` 与关键状态如 `CSR_RPC`、mask）
   - 先从 `simple`/`vecadd` 走通，再扩展到更复杂 kernel（如 rodinia）

## 5. lab/04 的阶段性计划（聚焦 instruction decode）

本 lab 的可交付物建议是一个“可复用的解码器 + 可对照的输出”，为后续翻译器提供稳定输入。

- [ ] 定义本项目的 `DecodedInst` 结构：`pc/len/op/rd/rs1/rs2/rs3/imm/is_vec/...`
- [ ] 建立 Ventus 指令编码表（优先从 spike `encoding.h` 自动提取；必要时手写补丁）
- [ ] 实现 32-bit 指令 decode（至少覆盖 `VentusInst_basic.txt` 当前需要关注的指令）
- [ ] 实现 `regext` 前缀合并（严格“一条前缀只作用于下一条指令”）
- [ ] 实现立即数抽取（I/S/B/U/J/Z/V...；可参考 cyclesim `decode.cpp`）
- [ ] 做一个最小 disassembler/pretty-printer，输出接近 spike/objdump 的文本用于对照
- [ ] 用现有样例做 golden test：
  - `testcases/simple/simple.vmem` + `testcases/simple/simple.dump`
  - `ventus-env/cyclesim/simple.vmem` / `ventus-env/cyclesim/object0.vmem` + 对应 `.dump`
- [ ] 记录已验证的指令子集与限制（例如：是否暂不支持 RVC、是否暂不支持 `regexti` 出现等）

> 观察（来自现有 dump，仅作参考）：`ventus-env/cyclesim/object0.dump` 中 `setrpc` 的 `rd` 均为 `zero`，因此 cyclesim 在 `exec_csr.cpp` 中将其写回值置 0 并不影响这些样例；但 SBT 侧仍建议按 Spike 语义实现“可选写回”以避免踩坑。
