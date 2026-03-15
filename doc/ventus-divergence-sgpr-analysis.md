# Ventus LLVM 对 `vbranch` / `join` 下标量寄存器有效性的处理分析

## Takeaway

先给结论：

1. Ventus LLVM **没有**把 `vbranch` / `join` 当成类似标量 `call` / `ret` 的边界来处理。
   没看到任何“在 `vbranch` 前保存一批 SGPR，在 `join` 后恢复”的编译器逻辑。

2. Ventus LLVM 也**没有**采用“进入 divergence 后所有标量寄存器全部失效，必须手工搬到向量寄存器”的粗暴模型。
   编译器的视角更精细：**语义上可能变成 lane-varying 的 SSA 值会走 VGPR 路径；保持 warp-uniform 的值可以留在 SGPR。**
   这里真正需要额外提醒的例外，主要是反方向的：某些本来未必 divergent 的值，也会因为 ABI / lowering 策略被保守地按 VGPR 路径接入，例如某些非 kernel 设备函数形参。

3. 编译器真正采用的主方案是：
   **用 divergence analysis 在编译期区分 uniform / divergent 值，然后按这个结果选择寄存器类。**
   `join` 只负责控制流汇合位置的硬件标注；“值的有效性”问题主要靠：
   - `N->isDivergent()` 判定
   - `getRegClassFor(..., isDivergent)`
   - join 点 mixed-class PHI 的 `GPR -> VGPR` 合法化修复
   - 若干后续 peephole / legalization pass

4. 因而，对你的三个猜测，最接近事实的是“**其他方案**”：
   **SGPR 有效性不是在 `vbranch/join` 上用 caller-saved/callee-saved 解决，也不是一刀切全部作废，而是靠 divergence analysis 与寄存器类别选择来保证。**
   对语义上会带着 lane-varying 语义流动的值，编译器会把它们放到 VGPR 路径；例外主要是 ABI / formal-argument lowering 额外把一些并不语义 divergent 的值也放进 VGPR，而不是相反。

5. 对“硬件是否保证在 divergent 区域两侧各执行一遍后，未升级为 VGPR 的 SGPR 仍然一致”这个更细的问题，**当前 LLVM 源码里看不到它依赖这种硬件保证**。
   相反，源码体现出的编译器保证是：
   **凡是会因为 warp divergence 而变成 lane-varying、并且要跨 `vbranch/join` 存活的 SSA 值，都不应继续以 SGPR 形式存在。**
   因而真正跨过 join 还保留为 SGPR 的值，在编译器抽象里就应当已经是 uniform；本报告看到的例外只体现为“uniform 也可能被保守放进 VGPR”，而不是“divergent 还能留在 SGPR”。

一句话概括：

> 从 Ventus LLVM 的抽象看，`join` 处理的是控制流 reconvergence；寄存器有效性问题则通过“需要跨 divergence 继续携带 lane-private 语义的值，不能继续作为 SGPR 存在”这一约束来解决。

---

## 分析范围与方法

本报告只分析当前仓库中的 Ventus LLVM 后端源码，重点看：

- `llvm/llvm/lib/Target/RISCV/Ventus*.cpp`
- `llvm/llvm/lib/Target/RISCV/Ventus*.td`
- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp`
- `llvm/llvm/lib/Target/RISCV/RISCVRegisterInfo.*`
- `llvm/llvm/lib/Target/RISCV/RISCVTargetMachine.cpp`

注意：

- 这里讨论的是**编译器如何建模**，不是硬件微架构手册的最终语义。
- 如果硬件内部真有更细粒度的“标量路径跟着两边跑时如何保证 SGPR 值”的机制，LLVM 并**没有显式建模**那套动态规则；LLVM 只建模 uniform/divergent。

---

## 1. 后端从一开始就把 SGPR / VGPR 当作两条不同语义路径

### 1.1 Ventus 明确有两套寄存器文件

`VentusRegisterInfo.td` 开头直接写明：

- `x0-x63` 是 `sALU` 的通用寄存器
- `v0-v255` 是 `vALU` 的通用寄存器

证据：

- `llvm/llvm/lib/Target/RISCV/VentusRegisterInfo.td:10-12`
- `llvm/llvm/lib/Target/RISCV/VentusRegisterInfo.td:252-259`
- `llvm/llvm/lib/Target/RISCV/VentusRegisterInfo.td:519-526`

其中：

- `GPR` 带 `IsSGPR = 1`
- `VGPR` 带 `IsVGPR = 1`

这不是普通 RISC-V 单寄存器文件的做法，而是后端从 register class 层面就把“uniform path”和“divergent path”分开了。

### 1.2 LLVM 用 `isDivergentRegClass()` 区分寄存器类别实现，但这不等于语义定义

`RISCVRegisterInfo.h` 里：

- `isSGPRClass`
- `isVGPRClass`
- `isDivergentRegClass`

其中 `isDivergentRegClass` 的实现是：

```cpp
return !isSGPRClass(RC);
```

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVRegisterInfo.h:54-65`
- `llvm/llvm/lib/Target/RISCV/RISCVRegisterInfo.h:90-92`

这更适合解读为：

> **在 Ventus/RISCV 后端实现里，SGPR-only 类被当作 uniform-friendly 类；非 SGPR-only 类会被更保守地视为“divergent reg class”。**

但不能把它直接上升成严格语义规则：

> “语义上 divergent 的值 = 所有非 SGPR 类值”

因为这里混入了寄存器类设计与 lowering 细节。例如：

- `getRegClassFor(..., false)` 在非 divergent 情况下仍可能返回 `GPRF32`
- 而 `GPRF32` 并不是 `isSGPRClass(RC)` 意义下的 SGPR-only 类

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVRegisterInfo.h:90-92`
- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:13580-13590`

因此，后文凡是讨论“值在语义上是否 divergent”，应以前端/IR 层 divergence analysis 与 `getRegClassFor(..., isDivergent)` 为主，而不应把 `!isSGPRClass` 本身当成语义定义。

### 1.3 TableGen pattern 直接按 `N->isDivergent()` 选 uniform / divergent 指令

`VentusInstrInfo.td` 很关键。它定义了：

- `UniformUnaryFrag`
- `UniformLoadFrag`
- `UniformBinFrag`
- `DivergentUnaryFrag`
- `DivergentBinFrag`
- `DivergentPrivateLoadFrag`
- `DivergentPrivateStoreFrag`

这些 pattern fragment 直接看 `N->isDivergent()` 决定选哪一类指令。

证据：

- `llvm/llvm/lib/Target/RISCV/VentusInstrInfo.td:23-37`
- `llvm/llvm/lib/Target/RISCV/VentusInstrInfo.td:39-55`
- `llvm/llvm/lib/Target/RISCV/VentusInstrInfo.td:57-99`

这已经说明后端设计哲学不是“先都当标量，到了 join 再补救”，而是：

> 在指令选择阶段就把 uniform 和 divergent 代码路径分开。

### 1.4 这里的 `uniform` 不是“从没出现在 divergent 区域里”

这一点很容易被误解。

更严格地说，这里的 `uniform` 指的是：

> **对当前活跃 lanes 来说，这个值仍表示同一个 warp-uniform 结果，因此可以只存一份在 SGPR 里。**

它并不等价于下面这些更强说法：

- “这个值从未出现在 divergent 控制流内部”
- “这个值从未在 divergent 区域里被写过”
- “这个值的所有 use 都一定 uniform”

LLVM 的 divergence analysis 明确区分：

- `isDivergent(const Value&)`：值在定义处是否 divergent
- `isDivergentUse(const Use&)`：某个 use 是否 divergent

而且头文件直接写了：

> Uses of a uniform value can be divergent.

证据：

- `llvm/llvm/include/llvm/Analysis/DivergenceAnalysis.h:77-82`
- `llvm/llvm/include/llvm/Analysis/DivergenceAnalysis.h:171-180`
- `llvm/llvm/lib/Analysis/LegacyDivergenceAnalysis.cpp:367-371`

---

## 2. 编译器如何判定“这个值不能再待在 SGPR 里”

### 2.1 divergence 来源被显式编码

`RISCVTargetLowering::isSDNodeSourceOfDivergence` 直接定义了哪些东西会让值变成 divergent：

- 从 VGPR 读出来的值
- `PRIVATE_ADDRESS` 的 load
- 往 `VGPRSpill` / private memory 的 store
- `CALLSEQ_END`
- 某些 intrinsic
- `vastart`
- read-modify-write atomic

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:13522-13575`

特别值得注意的点：

- `CopyFromReg` 如果来源寄存器是 VGPR，就被视作 divergent
- private memory load/store 被视作 divergent
- `CALLSEQ_END` 直接算 divergence 来源

这意味着一旦值和 per-thread/private state、调用结果、atomic 等发生关系，编译器就会保守地把它看成 lane-varying。

### 2.2 divergence analysis 不是只看“有没有被写”，而是沿数据依赖和控制依赖传播

这点非常关键。LLVM 并不是只做这种简单判断：

- “这个值有没有在 divergent 路径里被显式改写”

它实际做的是 SSA 传播分析。

旧版 `LegacyDivergenceAnalysis` 里：

1. **如果 branch 是 divergent，那么其 immediate post-dominator 中的 PHI 都变成 divergent**
2. **divergent 值沿 def-use 链继续传播**
3. **控制/同步依赖也会把影响区域外的 use 污染为 divergent**

证据：

- `llvm/llvm/lib/Analysis/LegacyDivergenceAnalysis.cpp:148-210`
- `llvm/llvm/lib/Analysis/LegacyDivergenceAnalysis.cpp:260-265`

新版 `DivergenceAnalysis` 里同样如此：

- `pushUsers` 对已知 divergent 值沿 users 继续传播
- `analyzeControlDivergence` 会找 divergent terminator 的 join blocks
- `taintAndPushPhiNodes` 会把 join block 中的非恒定 PHI 全标成 divergent

证据：

- `llvm/llvm/lib/Analysis/DivergenceAnalysis.cpp:135-155`
- `llvm/llvm/lib/Analysis/DivergenceAnalysis.cpp:265-304`
- `llvm/llvm/lib/Analysis/DivergenceAnalysis.cpp:312-329`

### 2.3 一旦值被判定为 divergent，主寄存器类选择会从 SGPR 改成 VGPR

`RISCVTargetLowering::getRegClassFor(MVT VT, bool isDivergent)` 是核心。

逻辑是：

- 默认先拿基础 reg class
- 如果值本来不是 SGPR 类、但 `isDivergent == false`，可以回落到 `GPR/GPRF32`
- **如果值本来是 SGPR 类，但 `isDivergent == true`，直接返回 `VGPRRegClass`**

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:13578-13590`

这是整个问题最关键的一条源码证据。它可以直接支持这样一个结论：

> 对原本属于 SGPR 类的值，只要编译器判定它是 divergent，就会把它改到 `VGPRRegClass`；因此这类值不会以 SGPR 形式跨过 `vbranch/join` 继续携带 thread-specific 语义。

### 2.4 kernel 参数与普通设备函数参数不是一刀切的

前面提到，`unpackFromRegLoc` 有一条很强的后端倾向：

- 设备侧参数接入机器层时，经常直接按 `isDivergent = true` 走 VGPR 路径

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:11782-11815`

但 IR 层 TTI 还有一个重要边界：

- **kernel 入口参数不是 divergence source**
- **普通设备函数参数则保守地当成 divergence source**

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVTargetTransformInfo.cpp:360-367`

具体逻辑是：

- `CallingConv::SPIR_KERNEL` / `CallingConv::VENTUS_KERNEL` 的 `Argument` 返回 `false`
- 其他函数参数返回 `true`

所以更完整的说法应是：

> Ventus LLVM 对参数并不是完全同一种保守策略，而是 **对 kernel 入口参数更信任其 uniform 性，对普通设备函数参数更保守**。

### 2.5 设备函数参数也被强制按 divergent 路径接入 VGPR

`unpackFromRegLoc` 里有非常直白的注释：

```cpp
// Setting isDivergent = true is essential to use VGPR
```

然后它用：

```cpp
const TargetRegisterClass *RC = TLI.getRegClassFor(LocVT.getSimpleVT(), true);
```

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:11782-11815`

这说明后端对**非 kernel 设备函数**的寄存器形参/返回值有一个很强的实现倾向：
**即使 IR 层语义未必已经证明其 divergent，也会因为 ABI / lowering 策略而保守地走 VGPR 路径。**

因此不能把“进入 VGPR”简单概括成“只由 divergence analysis 决定”。

---

## 3. `join` 在 LLVM 里只处理控制流，不处理“标量寄存器保存/恢复”

### 3.1 `VentusInsertJoinToVBranch` 做的事非常有限

这个 pass 的文件头注释写得很清楚：

> 如果生成了 `VBranch`，需要插入 `setrpc` 和 `join` 告诉硬件在哪汇合。

证据：

- `llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp:9-10`

具体逻辑：

1. 找到 `VBEQ/VBNE/VBLT/...`
2. 找该块的 post-dominator
3. 在 branch 前插 `AUIPC + SETRPC`
4. 在 post-dominator 块头插 `JOIN`

证据：

- `llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp:79-106`
- `llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp:110-154`

这里**完全没有**：

- spill SGPR
- restore SGPR
- 建 caller-saved / callee-saved 边界
- 给某些 SGPR 打特殊 liveness mask

也就是说，LLVM 把 `join` 看成 **control-flow reconvergence marker**，不是寄存器保存点。

### 3.2 指令定义也体现不出 call-like 语义

`VentusInstrInfoV.td` 中：

- `VBEQ..VBGEU` 只声明 `Defs = [X6]`
- `SETRPC` 声明 `Defs = [RPC]`
- `JOIN` 只是一个普通有 side effects 的指令定义

证据：

- `llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td:750-767`

尤其注意：

- branch 只把 `X6` 当 scratch
- `SETRPC` 写 `RPC`
- `JOIN` 本身没有携带任何“保存/恢复哪些 SGPR”的 ABI 信息

从后端接口设计上，它也不像一个 call boundary。

---

## 4. join 点 mixed-class PHI 的合法化：`VentusFixMixedPHI`

### 4.1 `VentusFixMixedPHI` 的设计目标就是修“VGPR 结果 + GPR 输入”

文件注释直接写了：

> 修复 PHI：结果是 VGPR，但输入来自 GPR/GPRF32/FPR。
> 在前驱块插入转换，把非 VGPR 输入搬到 VGPR，使 PHI 合法。

证据：

- `llvm/llvm/lib/Target/RISCV/VentusFixMixedPHI.cpp:9-12`

### 4.2 它处理的是“结果已经是 VGPR 的 PHI”的合法化

`processPHINode` 的逻辑：

1. 只处理“PHI 结果是 VGPR”的情况
2. 检查其输入是否有 `SGPR/FPR/GPRF32`
3. 如果有，则在对应前驱块 terminator 前插转换
4. 更新 PHI 操作数，改用新建的 VGPR

证据：

- `llvm/llvm/lib/Target/RISCV/VentusFixMixedPHI.cpp:75-127`

这说明它负责的是：

> **当上游语义判定已经让 join 结果成为 VGPR 时，把机器 PHI 的输入也修成一致的 VGPR 形式。**

它**不会**做下面这些更强的事情：

- 不会重新判断一个 join PHI 在语义上是不是 divergent
- 不会把“本来误留在 GPR 的 join PHI 结果”重新分类成 VGPR

因此，这个 pass 的角色应理解为 **machine-level legalization / repair**，而不是 divergence 语义的主要判定器。

这意味着在一个典型的 divergence/join 模式下：

```c
if (cond_lane) x = a;
else           x = b;
use(x);
```

编译器的视角不是“让同一个 SGPR 在 then/else 两侧切换保存恢复”，而是：

- `x` 在 join 后已经不再是 uniform 值
- join 后的 `x` 对应的 PHI 结果必须是 VGPR
- 如果某个前驱给它的是 GPR/FPR 形式，就在前驱块先转成 VGPR

这是一个非常标准的“SSA + divergence-aware regclass”方案。

### 4.3 这也解释了为什么“标量路径跟着两边执行”不会逼编译器做 SGPR 多版本

因为在 LLVM 的抽象里，**一旦 divergence analysis 已经认定某个 join 后值需要 lane-varying 语义，它就不该再是 SGPR；`VentusFixMixedPHI` 只是把这一判定落到合法的机器 PHI 上。**

所以它不需要：

- 在 SGPR 里搞 then 版本 / else 版本
- 在 `join` 时做 SGPR 选择恢复

它直接让值在 join 前就进入 VGPR 语义域。

---

## 5. 后续 pass 明确承认“divergent 节点里 sGPR 会被搬到 VGPR”

`VentusVVInstrConversion.cpp` 文件头注释非常关键：

> 当前 `sGPR` / `sGPRF32` 里的对象在 divergent nodes 中会被 moved to VGPR，
> 导致原本匹配 `VX/VF` 的 pattern 匹配不到，所以此 pass 把某些 `vv` 指令再改写回 `vx/vf`。

证据：

- `llvm/llvm/lib/Target/RISCV/VentusVVInstrConversion.cpp:9-12`

这是整个仓库里对本问题最直接的一句源码注释。

它等于明确承认了这样一种编译器实现倾向：

> **divergent 节点里，原本在 sGPR 的对象会被搬到 VGPR。**

这个 pass 的工作不是做 correctness，而是做后续 peephole：

- 发现 `%vgpr = COPY %gpr`
- 紧跟一个 `vop.vv`
- 就把它改成 `vop.vx/vf`

证据：

- `llvm/llvm/lib/Target/RISCV/VentusVVInstrConversion.cpp:165-176`
- `llvm/llvm/lib/Target/RISCV/VentusVVInstrConversion.cpp:178-227`
- `llvm/llvm/lib/Target/RISCV/VentusVVInstrConversion.cpp:250-255`

因此它是一个非常强的旁证：

- 正确性层面，前面已经允许/要求 `SGPR -> VGPR`
- 优化层面，再尽量把一些不必要的“先搬到 VGPR 再做 vv”折回到 `vx/vf`

这不是“SGPR 在 divergence 里继续保持有效”，而是“**实现主路径会把受 divergence 影响的对象抬升到 VGPR**”。

---

## 6. `caller-saved / callee-saved` 只出现在真正函数调用上，不出现在 `vbranch/join`

### 6.1 Ventus 的调用约定是独立话题

`VentusCallingConv.td` 明确说：

- Ventus calling convention 由 `CC_Ventus` 自定义处理
- 除 `ra` 外，`sGPR` 不需要 callee-saved
- callee-saved 主要是 `X1` 和 `V32-V255`

证据：

- `llvm/llvm/lib/Target/RISCV/VentusCallingConv.td:13-19`

这里值得注意：

- 调用 ABI 的 preserved set 是**函数调用**规则
- 不是 divergence/join 规则

### 6.2 `LowerCall` 里 preserved mask 只在 call 上附加

`LowerCall` 中：

- 会取 `getCallPreservedMask`
- 把 register mask operand 挂到 `CALL`

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:12133-12139`
- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:12396-12401`

而 `VentusInsertJoinToVBranch` 给 `vbranch/join` 插入的只是 `AUIPC/SETRPC/JOIN`，没有任何 regmask。

所以答案很明确：

> `vbranch/join` 不按 caller-saved / callee-saved 模式处理。

### 6.3 设备函数 ABI 也主要用 VGPR 传参

`CC_Ventus` 直接把 `V0-V31` 定义为参数寄存器。

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp:11446-11549`

这进一步说明设备侧通用数据路径本来就强依赖 VGPR，而不是试图让大量“可能 lane-varying 的值”长期待在 SGPR。

---

## 7. 双栈模型存在，但它按寄存器类别/地址空间分，不按 branch side 分

Ventus 后端确实有两套 spill 栈：

- `SGPRSpill`
- `VGPRSpill`

证据：

- `llvm/llvm/lib/Target/RISCV/RISCV.h:119-135`

Frame lowering 里也明确：

- `SGPRSpill` 用 `SP(X2)`
- `VGPRSpill` 用 `TP(X4)`

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp:376-382`
- `llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp:511-518`
- `llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp:585-589`

`determineStackID` 还会根据地址空间把对象分到不同 stack：

- `PRIVATE_ADDRESS -> VGPRSpill`
- 其他默认 `SGPRSpill`

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp:796-810`

这说明后端确实区分：

- uniform/标量 spill
- per-thread/向量 spill

但这仍然不是“在 then/else 两路之间为 SGPR 建两个版本”的机制。
它只是说明：**一旦值已经被分类为 VGPR 语义，它的栈位置也会跟着进入 VGPR/private 栈体系。**

---

## 8. Pass pipeline 也印证了这个设计

Ventus 专用 pass 顺序大致是：

1. `VentusPromoteAlloca`
2. ISel
3. `VentusFixMixedPHI`
4. Pre-RA: `VentusVVInstrConversion`, `VentusLegalizeLoad`
5. Pre-Emit: `VentusInsertJoinToVBranch`

证据：

- `llvm/llvm/lib/Target/RISCV/RISCVTargetMachine.cpp:224-240`
- `llvm/llvm/lib/Target/RISCV/RISCVTargetMachine.cpp:260-263`
- `llvm/llvm/lib/Target/RISCV/RISCVTargetMachine.cpp:289-295`
- `llvm/llvm/lib/Target/RISCV/RISCVTargetMachine.cpp:320-325`

这个顺序很有说服力：

- **先**完成指令选择和 PHI/寄存器类别修复
- **后**才在 pre-emit 阶段插 `SETRPC/JOIN`

这说明编译器认为：

> “值该在 SGPR 还是 VGPR”这个问题，应在 join 插入之前就解决；
> `join` 不是寄存器语义修复点，只是最终控制流标注点。

---

## 9. 对你提出的三个方案逐条回答

### 9.1 `vbranch/join` 是否按类似标量 func call 的 caller-saved / callee-saved 处理？

**不是。**

理由：

- `VentusInsertJoinToVBranch` 只插 `SETRPC/JOIN`，不插 save/restore，也不附 regmask
  见 `VentusInsertJoinToVBranch.cpp:79-106`
- call-preserved mask 只在 `LowerCall` 构造 `CALL` 节点时附加
  见 `RISCVISelLowering.cpp:12396-12401`
- `VentusCallingConv.td` 明确谈的是函数 ABI，而不是 branch/join
  见 `VentusCallingConv.td:13-19`

因此不能把 `join` 理解成“一个轻量级 scalar call-return”。

### 9.2 标量寄存器是否全部失效，必须把需要传递的数据拷贝到向量寄存器？

**也不是全部。**

更准确的说法是：

- **语义上会变成 divergent 的值**，编译器会把它放到 VGPR 语义域
- **仍保持 uniform 的值**，可以继续保留在 SGPR

理由：

- TableGen pattern 明确区分 `N->isDivergent()` 和 `!N->isDivergent()`
  见 `VentusInstrInfo.td:23-99`
- `getRegClassFor(..., true)` 会把原本 SGPR 类值改成 `VGPRRegClass`
  见 `RISCVISelLowering.cpp:13580-13588`

所以不是“全失效”，而是“**只让不再 uniform 的值退出 SGPR；额外存在的 ABI/lowering 例外，是把一部分仍可 uniform 的值也保守地放进 VGPR**”。

### 9.3 实际方案是什么？

**实际方案是“divergence-aware register class discipline”**：

1. 编译期识别 divergence 来源
   见 `RISCVISelLowering.cpp:13522-13575`
2. divergent 值分配到 VGPR
   见 `RISCVISelLowering.cpp:13580-13588`
3. join 点若出现 `VGPR PHI <- GPR inputs`，在前驱块插转换
   见 `VentusFixMixedPHI.cpp:106-127`
4. 最后只把 `vbranch` 的 reconvergence 信息编码成 `SETRPC/JOIN`
   见 `VentusInsertJoinToVBranch.cpp:79-106`

这套方案本质上是：

> **不用在动态 SIMT stack 上维护“SGPR 哪个分支版本当前有效”，而是在 SSA/寄存器分配阶段就避免这种情况出现。**

---

## 10. 关于“硬件是否保证未升级 SGPR 一致性”的进一步澄清

这一点值得单独说清楚。

### 10.1 从当前 LLVM 源码看，编译器并不像是在依赖硬件兜底

也就是说，LLVM 里看不到这样的建模：

- “即使一个值在 divergent 区域内走了 then/else 两侧，只要它还在 SGPR，硬件会自动帮我保证它语义一致”

相反，LLVM 的建模是：

- **不要让需要这种兜底的问题以 SGPR 形式出现**

也就是：

- 先用 divergence analysis 判断某个值是否会因为控制/数据依赖而变成 divergent
- 如果会，就让它进入 VGPR 语义域
- 如果 join 点仍有 `GPR -> VGPR` 的残留不一致，再用 `VentusFixMixedPHI` 补齐

因此，从编译器视角可以说：

> 不是“硬件保证跨 join 的 SGPR 一定正确”，而是“编译器会避免让可能跨 join 变成 lane-varying 的值继续作为 SGPR 活着”。

### 10.2 你的猜想大体正确，但要避免说得过强

下面这句话：

> 编译器可能做好了前期的分析，确定一个值在其生命周期内，绝对不会被 warp divergence 地写入，才会置入 sgpr

这个方向是对的，但更严谨的表述应改成：

> 编译器会保证：**凡是要跨 `vbranch/join` 延续、并且其值会因 divergent 控制流而在 lane 间不同的 SSA 值，都不会继续留在 SGPR。**

原因是“不会被 warp divergence 地写入”这个说法容易让人误解成：

- divergent 区域内部完全不能出现对 SGPR 的写

这就太强了。LLVM 源码并不支持这么强的结论。

### 10.3 更准确的说法：禁止的是“跨 join 仍以 SGPR 形式存在的 lane-varying 值”

换句话说，LLVM 要避免的是这种情况：

```c
if (lane_cond)
  s = 1;
else
  s = 2;
use(s);   // s 不能还被当成 SGPR
```

而不是避免这种更宽泛的情况：

- divergent 区域内部出现任何 SGPR 指令
- divergent 区域内部出现任何 SGPR 写

实际上，divergent 区域内部仍可能存在 SGPR 操作，只要这些值仍被证明是 uniform，或者只在局部路径内使用、不以“lane-varying 结果跨 join”形式流出。

### 10.3.1 哪些 SGPR 写在 divergent 区域内部仍可能是合理的

至少有下面几类，不应被误判为“编译器模型出错”：

1. **仍被分析为 uniform 的中间结果**
   即使代码处在 divergent 控制流块里，该值本身也未必因控制/数据依赖传播而变成 divergent。

2. **只在单一路径局部使用、不会以 lane-varying 结果跨 join 流出的值**
   这类值即使暂时仍在 SGPR，也不会把问题带到 join 之后。

3. **架构/指令约定要求的显式 side-effect 寄存器写**
   例如：
   - `VBEQ..VBGEU` 定义 `X6`
   - `SETRPC` 定义 `RPC`

   证据：

   - `llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td:750-767`

因此，不能把“在 divergent 区域里看见 SGPR 写”直接等同于“LLVM 依赖硬件去维护跨-join SGPR 多版本一致性”。

### 10.4 直接证据：divergent branch 的 join 点 PHI 会被标为 divergent

这一点是验证上面判断的关键。

`LegacyDivergenceAnalysis.cpp` 中明确写道：

- 如果一个 branch 是 divergent
- 那么它的 immediate post-dominator 中的 PHI 都是 divergent
- 除非该 PHI 实际上只是 constant/undef

证据：

- `llvm/llvm/lib/Analysis/LegacyDivergenceAnalysis.cpp:147-178`

`DivergenceAnalysis.cpp` 中也有同样逻辑：

- 对 divergent join point，taint 该 join block 中的所有非恒定 PHI

证据：

- `llvm/llvm/lib/Analysis/DivergenceAnalysis.cpp:265-286`
- `llvm/llvm/lib/Analysis/DivergenceAnalysis.cpp:289-304`

这说明 LLVM 的核心保证方式正是：

> **一旦某个值在 join 处通过 PHI 汇合，而且这个汇合受 divergent 控制依赖影响，该值就被看成 divergent。**

接下来它自然就会走前文说的那条路径：

- divergent 值用 VGPR
- 必要时在前驱块插 `VMV_V_X`

### 10.5 因此，“根本不存在这个问题”这句话只能在编译器抽象层面成立

如果你的意思是：

- “在 LLVM 编译器抽象里，不会让一个真正需要 lane-private 语义的跨-join 值继续留在 SGPR，因此不需要再讨论这种 SGPR 如何靠硬件保持一致性”

那这个判断是成立的。

但如果你的意思是：

- “硬件层面完全不存在 SGPR 一致性问题”

那从当前仓库源码无法证明，因为这里主要是编译器侧抽象，不是硬件规范。

进一步说，当前源码能可靠支持的只是：

- LLVM **没有显式依赖**硬件去兜底这种 SGPR 一致性问题
- LLVM **在编译期避免**把问题暴露给硬件

但当前源码**不能直接证明**下面这些更强命题：

- Ventus 硬件绝对不存在任何内部 SGPR 一致性机制
- 如果编译器误分类，把本应 divergent 的值留在 SGPR，硬件一定无法跑对
- `join` / SIMT stack 的微架构内部绝不会保存任何额外标量状态

因此更稳妥的最终说法是：

> **从 Ventus LLVM 的视角，这个问题基本被前移到了编译期解决：跨 `vbranch/join` 仍保留为 SGPR 的值，在编译器抽象里应当已经是 uniform；而需要携带 lane-varying 语义的值不会继续以 SGPR 形式流过 join。**

---

## 11. 一个简化示例来理解 LLVM 的视角

下面这个例子不是源码里的测试，而是基于上述实现得出的编译器视角：

```c
int x;
if (lane_cond)
  x = 1;
else
  x = 2;
use(x);
```

Ventus LLVM 更可能把它理解成：

1. `lane_cond` 是 divergent，所以控制流会用 `vbranch`
2. join 后的 `x` 不再 uniform，因此 `x` 对应的 PHI 结果必须是 VGPR
3. then/else 中如果 `1` / `2` 先以 GPR 形式出现，会在前驱块被插入 `GPR -> VGPR` 转换
4. `use(x)` 读的是 VGPR 版本
5. pre-emit 再插 `SETRPC/JOIN`

而**不会**理解成：

- “then 分支改一份 SGPR，else 分支再改另一份 SGPR，靠 join 恢复到正确版本”

---

## 12. 我认为最关键的源码证据

如果只看最重要的几处，我会按这个优先级排序：

1. `VentusVVInstrConversion.cpp:9-12`
   直接写了：`sGPR / sGPRF32` 在 divergent nodes 中会被 moved to VGPR。

2. `RISCVISelLowering.cpp:13580-13588`
   直接实现了：`isDivergent == true` 时把 SGPR 类值改到 `VGPRRegClass`。

3. `VentusFixMixedPHI.cpp:9-12` 与 `:106-127`
   直接说明 join 点 PHI 的合法化方式是把 GPR 输入搬成 VGPR。

4. `VentusInsertJoinToVBranch.cpp:79-106`
   直接说明 `join` 插入只处理控制流汇合目标，不处理寄存器保存恢复。

5. `VentusInstrInfo.td:23-99`
   直接说明 instruction selection 从一开始就用 `N->isDivergent()` 区分两条路径。

6. `LegacyDivergenceAnalysis.cpp:148-210` 与 `DivergenceAnalysis.cpp:265-304`
   直接说明 divergent branch 的 join 点 PHI 会被标成 divergent，而且 divergence 会沿控制依赖传播。

7. `DivergenceAnalysis.h:77-82,171-180`
   直接说明 LLVM 区分“值 divergent”和“use divergent”，不能把 uniform 简化成“从未出现在 divergent 区域”。

8. `RISCVTargetTransformInfo.cpp:360-367`
   直接说明 kernel 入口参数与普通设备函数参数在 divergence source 判定上是区别对待的。

---

## 最终结论

对“Ventus ISA 在 warp divergence - join 模式下如何处理标量寄存器有效性”这个问题，**从 Ventus LLVM 编译器的视角**，结论是：

- LLVM **不**把 `vbranch/join` 视为 caller-saved / callee-saved 边界。
- LLVM **不**假设“所有 SGPR 在 divergence 后全部作废”。
- LLVM 采用的是**更静态、更 SSA 化的方案**：
  - 语义上保持 warp-uniform 的值可以留在 SGPR，语义上需要 lane-specific 的值不会继续留在 SGPR
  - 但还存在 ABI / formal-argument lowering 等“非 semantic divergence 但仍走 VGPR”的例外
  - join 点若结果已经被判成 VGPR，则通过 VGPR PHI 及其 legalization 汇合值
  - `join` 本身只负责控制流 reconvergence 的硬件标注

如果把它压成一句工程化的话：

> Ventus LLVM 处理的不是“SGPR 在 `join` 时怎么恢复”，而是“让需要恢复的问题不要以 SGPR 形式出现”。
