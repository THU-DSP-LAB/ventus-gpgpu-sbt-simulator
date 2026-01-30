# Ventus Warp Divergence 与 SGPR 共享机制分析

## 1. “分岔/串行执行”的核心语义
在 Ventus 架构中（以 [ventus-env/spike/riscv/processor.cc](ventus-env/spike/riscv/processor.cc) 为准），`vbeq`/`vbne` 等指令触发 SIMT 分岔后，依赖 **SIMT Stack** 让两条路径按掩码（mask）串行执行。

### SIMT Stack 的局限性
该硬件栈 **只保存控制流状态**（PC 和 mask），不保存任何标量寄存器（x/SGPR）的状态：
- **分岔与压栈逻辑**：在 [ventus-env/spike/riscv/processor.cc](ventus-env/spike/riscv/processor.cc#L1169) 的 `push_branch` 函数中，当 `if_mask` 和 `else_mask` 都不为 0 时压栈，并根据有效位选择优先执行路径。
- **栈项结构**：如 [ventus-env/spike/riscv/processor.h](ventus-env/spike/riscv/processor.h#L623) 所示，栈项仅包含 `r_pc`/`r_mask`/`else_pc`/`else_mask` 等控制流信息。
- **合流（Join）逻辑**：当到达重收敛点（reconvergence pc）时，从栈顶弹出 `else_pc`/`else_mask` 并跳转，见 [ventus-env/spike/riscv/insns/join.h](ventus-env/spike/riscv/insns/join.h#L1)。
- **指令实现**：`vbeq` 等指令通过调用 `push_branch` 更新 PC，参考 [ventus-env/spike/riscv/v_ext_macros.h](ventus-env/spike/riscv/v_ext_macros.h#L2343)。

**结论**：由于两个分支在同一个 Warp 上串行执行且共享同一套 SGPR，如果分支 A 修改了某个 SGPR 而分支 B 随后需要使用其原始值，由于 SIMT Stack 不负责恢复 SGPR，可能会引发数据错误。

---

## 2. 编译器侧的应对策略：数据分歧化（Divergence Analysis）
Ventus LLVM 后端并未在硬件层实现 SGPR 的自动保存/恢复，而是通过静态分析确保“跨分岔生存的值”不落在 SGPR 中，而是分配到 VGPR。

### 核心处理流程：
1. **IR 层分歧分析 (Divergence Analysis)**
   - 使用 LLVM 原生的 `LegacyDivergenceAnalysis` 传播数据和同步依赖，见 [ventus-env/llvm/llvm/lib/Analysis/LegacyDivergenceAnalysis.cpp](ventus-env/llvm/llvm/lib/Analysis/LegacyDivergenceAnalysis.cpp#L146)。
   - 在 RISC-V TTI 中对分歧源（Divergence Source）的定义非常激进（例如直接将 PHINode 视为源），见 [ventus-env/llvm/llvm/lib/Target/RISCV/RISCVTargetTransformInfo.cpp](ventus-env/llvm/llvm/lib/Target/RISCV/RISCVTargetTransformInfo.cpp#L360)。

2. **寄存器类选择 (VGPR vs SGPR)**
   - 在 SelectionDAG 构建虚拟寄存器时，直接根据分析结果决定：`divergent` 值进入 VGPR，`uniform` 值进入 SGPR (GPR)。
   - 相关逻辑参考 [ventus-env/llvm/llvm/lib/CodeGen/SelectionDAG/FunctionLoweringInfo.cpp](ventus-env/llvm/llvm/lib/CodeGen/SelectionDAG/FunctionLoweringInfo.cpp#L396) 和 [ventus-env/llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp](ventus-env/llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp#L13580)。

3. **调用约定 (Calling Convention)**
   - Ventus CC 明确将参数寄存器设定为 `V0-V31`（参考 [ventus-env/llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp](ventus-env/llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp#L11446)），极大减少了重要程序值长期占用 SGPR 并在分支切换时被覆盖风险。

4. **分岔控制流指令生成**
   - 指令定义参考 [ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td](ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td#L750)。
   - 只有被判为 `divergent` 的条件分支才会生成 `VBEQ`/`VBNE` 等指令，见 [ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td](ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td#L1437)。
   - 编译器负责插入重收敛指令（`SETRPC` + `JOIN`），见 [ventus-env/llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp](ventus-env/llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp#L86)。

5. **混合类型 PHI 修复 (Mixed PHI Fixup)**
   - 这是 Ventus 特有的补丁：如果 PHI 节点的某个输入是 SGPR 但结果需要是 VGPR，会强制性在路径末尾插入同步指令，见 [ventus-env/llvm/llvm/lib/Target/RISCV/VentusFixMixedPHI.cpp](ventus-env/llvm/llvm/lib/Target/RISCV/VentusFixMixedPHI.cpp#L1)。
   - 同时，后端明确禁止 VGPR 向 SGPR 的直接拷贝，强制要求必须通过广播（Broadcast）方式，见 [ventus-env/llvm/llvm/lib/Target/RISCV/RISCVInstrInfo.cpp](ventus-env/llvm/llvm/lib/Target/RISCV/RISCVInstrInfo.cpp#L217)。

---

## 3. 总结
Ventus 解决“分支间 SGPR 冲突”的逻辑并非依靠硬件 Save/Restore，而是靠：
1. **Divergence Analysis** 识别可能受控制流影响的所有值。
2. **强制类型提升**：将所有跨控制流合并或可能分歧的 SSA 值分配到 **VGPR**。
3. **硬件 Mask 保护**：VGPR 每 Lane 独立且受当前执行 Mask 保护，从而天然避免了串行路径间的数据污染。

如果你发现某个 Kernel 出现了 SGPR 被覆盖导致的逻辑错误，通常意味着该变量被错误地标记为了 `uniform` 或是由于某种优化导致它在应该进入 VGPR 时仍留在了 SGPR。
