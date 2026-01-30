# Ventus 定制 LLVM（`ventus-env/llvm/`）速查概述

本文面向本仓库内的 `ventus-env/llvm/`（Ventus GPGPU 定制 LLVM/Clang/libclc）做“功能分层 + 代码索引”式概览，目标是之后需要改功能时能更快定位到对应实现文件。

> 信息来源均来自当前代码树内的实际文件与注释（例如 `ventus-env/llvm/README.md`、`ventus-env/llvm/llvm/lib/Target/RISCV/` 等）。

---

## 1. 这套编译器解决什么问题

`ventus-env/llvm/` 是一个 LLVM monorepo 风格的工程（含 `llvm/`、`clang/`、`lld/`、`libclc/` 等），其中 **Ventus 的实现主要落在 RISC-V 后端与 OpenCL 相关 ABI/内建支持**：

- 目标 CPU：`-mcpu=ventus-gpgpu`（见 `ventus-env/llvm/llvm/lib/Target/RISCV/RISCV.td` 与 `ventus-env/llvm/llvm/include/llvm/Support/RISCVTargetParser.def`）。
- 面向 OpenCL C：README 给出 OpenCL 编译示例（见 `ventus-env/llvm/README.md`）。
- Ventus 的“向量/线程”语义：在 RISC-V V 扩展编码基础上做 SIMT/vALU 定制（大量 TableGen 文件以 `Ventus*` 命名，位于 `ventus-env/llvm/llvm/lib/Target/RISCV/`）。

---

## 2. 快速理解：从 OpenCL 到 Ventus ELF 的链路

以 `ventus-env/llvm/README.md` 的示例为主线，典型链路是：

1) OpenCL C → LLVM IR（Clang 前端）
- `./install/bin/clang -S -cl-std=CL2.0 -target riscv32 -mcpu=ventus-gpgpu -emit-llvm ...`

2) LLVM IR → Ventus/RISC-V 目标代码（LLVM 后端）
- `./install/bin/llc -mtriple=riscv32 -mcpu=ventus-gpgpu ...`

3) 链接（需要 Ventus 定制链接脚本 + libclc/crt0）
- `utils/ldscripts/ventus/elf32lriscv.ld`：README 明确说明为了满足 spike 的地址空间需求使用了定制脚本。
- `libclc/riscv32/lib/*`：提供 `crt0.S`、work-item/work-group 支撑与 Ventus 相关头文件。

你后续定位问题时，可以把问题按阶段拆分：前端（类型/地址空间/ABI）→ IR（intrinsic/CC/pass）→ CodeGen（指令选择/寄存器/栈/机器层 pass）→ 链接/运行时。

---

## 3. Ventus 相关改动：按层/按目录的结构化索引

### 3.1 Clang（前端/ABI/OpenCL 相关）

**(1) Ventus 专用 RISC-V ABI 与 OpenCL kernel calling convention**

- `ventus-env/llvm/clang/lib/CodeGen/TargetInfo.cpp`
  - `VentusRISCVABIInfo`：为 `ventus-gpgpu` 选择不同的参数/返回值分类规则（含对 OpenCL kernel vs 非 kernel 的差异处理）。
  - `getOpenCLKernelCallingConv()`：当 `CPU == "ventus-gpgpu"` 时返回 `llvm::CallingConv::VENTUS_KERNEL`。
  - `emitVoidPtrDirectVAArg()`：针对 Ventus 调整 varargs 的栈增长方向（注释写明 “In ventus, the stack grow upwards”）。

**(2) OpenCL 地址空间映射**

- `ventus-env/llvm/clang/lib/Basic/Targets/RISCV.cpp`
  - `VentusAddrSpaceMap`：OpenCL 的 `global/local/constant/private` 地址空间映射。
  - `RISCVTargetInfo::adjust()`：设置 `UseAddrSpaceMapMangling`、更新 DataLayout，并开启 OpenCL 2.0+ 的 generic address space。

**(3) OpenCL barrier/subgroup barrier builtin → LLVM intrinsic**

- `ventus-env/llvm/clang/include/clang/Basic/BuiltinsRISCV.def`
  - Ventus OpenCL builtins：`barrier` / `work_group_barrier` / `sub_group_barrier`。
- `ventus-env/llvm/clang/lib/CodeGen/CGBuiltin.cpp`
  - 以上 builtin 被映射到 `Intrinsic::riscv_ventus_*` 系列 intrinsic（见后文 LLVM IR 层的 `IntrinsicsRISCV.td`）。

**(4) Driver 侧参数/行为微调**

- `ventus-env/llvm/clang/lib/Driver/ToolChains/Clang.cpp`
  - 当 `CPU == "ventus-gpgpu"`：额外添加 `-fno-optimize-sibling-calls` 等选项（影响尾调用/栈行为）。

---

### 3.2 LLVM IR/通用层（CallingConv、intrinsic、通用优化开关）

**(1) 新 calling convention：`ventus_kernel`**

- `ventus-env/llvm/llvm/include/llvm/IR/CallingConv.h`
  - `CallingConv::VENTUS_KERNEL = 104`。
- LLVM IR 文本格式支持（`*.ll` 能打印/解析 `ventus_kernel`）
  - `ventus-env/llvm/llvm/lib/IR/AsmWriter.cpp`
  - `ventus-env/llvm/llvm/lib/AsmParser/LLParser.cpp`
  - `ventus-env/llvm/llvm/lib/AsmParser/LLLexer.cpp`
  - `ventus-env/llvm/llvm/include/llvm/AsmParser/LLToken.h`

**(2) Ventus barrier intrinsic 定义**

- `ventus-env/llvm/llvm/include/llvm/IR/IntrinsicsRISCV.td`
  - `int_riscv_ventus_barrier`
  - `int_riscv_ventus_barrier_with_scope`
  - `int_riscv_ventus_subgroup_barrier`
  - `int_riscv_ventus_subgroup_barrier_with_scope`

**(3) opt 工具与通用优化的小定制**

- `ventus-env/llvm/llvm/tools/opt/opt.cpp`
  - 将 `ventus-printf-runtime-binding` 纳入 NPM/Legacy 选择逻辑相关列表（方便用 `opt -passes=...` 调试该 pass）。
- `ventus-env/llvm/llvm/lib/CodeGen/SelectionDAG/DAGCombiner.cpp`
  - 注释写明 “For now there's no need to fold sub to add in ventus”，禁用了一个 `sub`→`add(-imm)` 的 fold（属于全局 CodeGen 行为微调）。

**(4) RISC-V 解析器/特性支持的 Ventus 相关改动**

- `ventus-env/llvm/llvm/include/llvm/Support/RISCVTargetParser.def`
  - 增加 `PROC(VENTUS_GPGPU, {"ventus-gpgpu"}, ...)`，并给出默认 `-march`（如 `rv32ima_zhinx_zfinx_zdinx_zve32f`）。
- `ventus-env/llvm/llvm/include/llvm/Support/TargetParser.h`
  - 注释说明为支持 `zve32*`（Ventus 需要）将 `RVVBitsPerBlock` 调整为 `32`。

---

### 3.3 LLVM RISC-V 后端（Ventus 核心：寄存器/指令/栈/机器层 pass）

Ventus 的“主战场”基本都在 `ventus-env/llvm/llvm/lib/Target/RISCV/` 目录内，既包含新增的 `Ventus*.{td,cpp,h}`，也包含对标准 RISC-V 文件（`RISCVISelLowering.cpp`、`RISCVFrameLowering.cpp` 等）的定制。

#### (1) 目标 CPU/特性入口

- `ventus-env/llvm/llvm/lib/Target/RISCV/RISCV.td`
  - `ProcessorModel<"ventus-gpgpu", ...>`：将 Ventus CPU 绑定到一组 feature（如 `FeatureStdExtZve32f` 等）。
  - `include "VentusRegisterInfo.td" / "VentusCallingConv.td" / "VentusInstrInfo.td"`：Ventus 的 TableGen 入口在这里被纳入 RISC-V target。

#### (2) OpenCL 地址空间与“多栈”约定（后端通用常量）

- `ventus-env/llvm/llvm/lib/Target/RISCV/RISCV.h`
  - `namespace RISCVAS`：定义 `GLOBAL/LOCAL/CONSTANT/PRIVATE` 地址空间编号（OpenCL/后端一致性很关键）。
  - `namespace RISCVStackID`：定义 `SGPRSpill`、`VGPRSpill`、`LocalMemSpill` 等 stack ID，并在注释里明确 Ventus 有“两套栈”。

#### (3) TableGen：Ventus 寄存器文件与指令集描述

- 寄存器/寄存器类
  - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusRegisterInfo.td`
    - 注释描述：`x0-x63`（sALU/SGPR）与 `v0-v255`（vALU/VGPR）。
    - 为寄存器类设置 `TSFlags`（`IsVGPR/IsSGPR/IsFGPR`），供后端在 C++ 层判断寄存器类属性（见 `RISCVRegisterInfo.h`）。
- 指令与匹配模式
  - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfo.td`
    - 关键点：大量 `PatFrag`/pattern 以 `N->isDivergent()` 为条件，把 **uniform vs divergent** 的选择逻辑编码进匹配规则。
    - 定义 Ventus 扩展指令：例如 `REGEXT/REGEXTI/ENDPRG`，以及 `barrier` 类指令等。
  - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td` 与相关 include（`VentusInstrFormatsV.td`、`VentusInstrInfoVPseudos.td`、`VentusInstrInfoV*Patterns.td`）
    - 描述 Ventus vALU 指令：在 RVV 编码框架上实现 SIMT 标量语义。

#### (4) Calling convention / 参数传递：机器层实现主要在 `RISCVISelLowering.cpp`

- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusCallingConv.td`
  - 注释写明：Ventus calling convention 由 `RISCVISelLowering.cpp (CC_Ventus)` 里的自定义代码处理。
  - callee-saved 规则：特别强调 sGPR 由 kernel 设置、无需常规 callee-saved（并引入 V32-V255 等保存集合）。
- `ventus-env/llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp`
  - `CC_Ventus`：实现 Ventus 参数传递（注释写明使用 `V0-V31` 作为参数寄存器）。
  - 这里通常也是你要改“参数如何放到 VGPR/SGPR、如何处理 varargs/返回值”的第一现场。

#### (5) 栈/FrameLowering：多栈、SP/TP 与私有内存

- `ventus-env/llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp`
  - `getFrameIndexReference()`：根据 `RISCVStackID` 选择不同的 FrameReg（例如 `VGPRSpill` vs `SGPRSpill`）。
  - `eliminateCallFramePseudoInstr()`：注释提到 Ventus kernel 与普通函数栈寄存器不同，并对 `TP`/私有内存基址做一致性处理。
- `ventus-env/llvm/llvm/lib/Target/RISCV/RISCVRegisterInfo.h`
  - `RISCVRCFlags` 与 `isVGPRClass/isSGPRClass/isFPRClass`：寄存器类属性判断（依赖 TableGen 设置的 `TSFlags`）。
  - `getPrivateMemoryBaseRegister()` 等：私有内存寻址相关的约定入口。

#### (6) 资源用量统计与输出：`.ventus.resource.*` section

Ventus 似乎会把每个 entry function（kernel）的资源信息写进 ELF 的专用 section，供运行时/driver 读取。

- 资源信息结构：
  - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusProgramInfo.h`
    - `VentusProgramInfo/SubVentusProgramInfo`：统计 VGPR/SGPR/LDS/PDS 使用量。
- 统计时机：
  - `ventus-env/llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp`
    - `processFunctionBeforeFrameFinalized()`：遍历指令/操作数，填充 `VentusProgramInfo`。
- 输出位置：
  - `ventus-env/llvm/llvm/lib/Target/RISCV/RISCVAsmPrinter.cpp`
    - 当 `MF.getInfo<RISCVMachineFunctionInfo>()->isEntryFunction()`：写入 `.ventus.resource.<func>` section（依次 emit VGPR/SGPR/LDS/PDS）。

#### (7) Ventus 相关新增/定制 passes（IR/Machine 层）

**注册与编译入口**

- `ventus-env/llvm/llvm/lib/Target/RISCV/CMakeLists.txt`
  - 将各个 `Ventus*.cpp` 编进 `RISCVCodeGen`。
- `ventus-env/llvm/llvm/lib/Target/RISCV/RISCVTargetMachine.cpp`
  - 在 `addIRPasses()` 中默认插入：
    - `createVentusPrintfRuntimeBinding()`
    - `createVentusPromoteAllocaPass()`

**各 pass 的定位与职责（按文件名）**

- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusPrintfRuntimeBinding.cpp`
  - ModulePass：将 `printf` lower 为对 printf buffer 的写入序列，并用 metadata/ID 表示 format string（便于运行时绑定）。
- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusPromoteAlloca.cpp`
  - FunctionPass：把 private address space 的 `alloca` 提升为向量（参考 AMDGPU 思路），减少 private memory/栈开销。
- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusRegextInsertion.cpp`
  - MachineFunctionPass：当寄存器编码值 > 31 时，在指令前插入 `REGEXT` 来扩展寄存器编码范围（解决指令编码位宽限制）。
- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusVVInstrConversion.cpp`
  - MachineFunctionPass：将部分 `vop.vv` 转换为 `vop.vx/vf`，以匹配后续 pattern（与 divergent 情况下的寄存器搬运策略有关）。
- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusFixMixedPHI.cpp`
  - MachineFunctionPass：修复“PHI 结果是 VGPR，但输入来自 GPR/FPR/GPRF32”的非法 PHI，通过在前驱块插入转换/拷贝使 PHI 合法。
- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp`
  - MachineFunctionPass：对 divergent branch（`VBEQ/VBNE/...`）插入 `SETRPC/JOIN`，让硬件知道分支的 join 点（依赖 post-dominator）。
- `ventus-env/llvm/llvm/lib/Target/RISCV/VentusLegalizeLoad.cpp`
  - MachineFunctionPass：针对 varargs 相关场景修正 `vlw12.*` 等指令合法性问题（注释里给了 Ventus vs 标准 RISC-V 的对比）。

---

### 3.4 libclc / 链接脚本（运行时/启动与 builtin 支撑）

**(1) Ventus CSR 与 kernel metadata buffer 约定**

- `ventus-env/llvm/libclc/riscv32/lib/ventus.h`
  - 定义 CSR 编号（如 `CSR_TID/CSR_WGID/CSR_LDS/CSR_PDS/CSR_PRINT` 等）。
  - 定义 kernel metadata buffer 的 layout 与偏移（`KNL_*`）。

**(2) crt0 与 work-item/work-group 支撑**

- `ventus-env/llvm/libclc/riscv32/lib/crt0.S`
- `ventus-env/llvm/libclc/riscv32/lib/workitem/workitem.S`
- `ventus-env/llvm/libclc/riscv32/lib/workgroup/wgbarrier.cl`
- `ventus-env/llvm/libclc/riscv32/lib/CMakeLists.txt`

**(3) 链接脚本**

- `ventus-env/llvm/utils/ldscripts/ventus/elf32lriscv.ld`
  - README 提到 `_start` 需要满足 spike 地址空间布局要求，因此使用定制链接脚本。

---

## 4. 常见“我该改哪里”：按需求倒排索引

- **改 OpenCL kernel 的 calling convention / 形参落在哪些寄存器**
  - Clang 侧：`ventus-env/llvm/clang/lib/CodeGen/TargetInfo.cpp`（`VentusRISCVABIInfo` + `getOpenCLKernelCallingConv()`）
  - LLVM IR 侧：`ventus-env/llvm/llvm/include/llvm/IR/CallingConv.h`（`VENTUS_KERNEL`）
  - 后端 侧：`ventus-env/llvm/llvm/lib/Target/RISCV/RISCVISelLowering.cpp`（`CC_Ventus`）
  - TableGen/约定：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusCallingConv.td`

- **改 barrier/work_group_barrier/sub_group_barrier 的语义或指令选择**
  - Clang builtin：`ventus-env/llvm/clang/include/clang/Basic/BuiltinsRISCV.def`
  - builtin → intrinsic：`ventus-env/llvm/clang/lib/CodeGen/CGBuiltin.cpp`
  - intrinsic 定义：`ventus-env/llvm/llvm/include/llvm/IR/IntrinsicsRISCV.td`
  - 指令/匹配：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfo.td`、`ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td`

- **改 VGPR/SGPR 寄存器数量、编码、寄存器类判定**
  - TableGen：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusRegisterInfo.td`
  - C++ 判定：`ventus-env/llvm/llvm/lib/Target/RISCV/RISCVRegisterInfo.h`
  - 编码溢出处理：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusRegextInsertion.cpp` + `REGEXT` 定义（`VentusInstrInfo.td`）

- **改 divergent 分支的 join 机制**
  - 机器层 pass：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusInsertJoinToVBranch.cpp`
  - 分支/汇合相关指令：从下列 TableGen 文件中搜索 `VBEQ/VBNE/JOIN/SETRPC`
    - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfo.td`
    - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoV.td`
    - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoVPseudos.td`
    - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoVSDPatterns.td`
    - `ventus-env/llvm/llvm/lib/Target/RISCV/VentusInstrInfoVVLPatterns.td`

- **改 printf 支持（OpenCL printf）**
  - IR lowering：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusPrintfRuntimeBinding.cpp`
  - 与运行时 buffer/CSR 相关：`ventus-env/llvm/libclc/riscv32/lib/ventus.h`（`CSR_PRINT`、`KNL_PRINT_*`）

- **改资源统计与输出（VGPR/SGPR/LDS/PDS 的写入规则）**
  - 数据结构：`ventus-env/llvm/llvm/lib/Target/RISCV/VentusProgramInfo.h`
  - 统计逻辑：`ventus-env/llvm/llvm/lib/Target/RISCV/RISCVFrameLowering.cpp`
  - ELF section 输出：`ventus-env/llvm/llvm/lib/Target/RISCV/RISCVAsmPrinter.cpp`（`.ventus.resource.<func>`）

---

## 5. 测试/样例位置（用于“改完先跑哪类用例”）

- LLVM 后端用例：`ventus-env/llvm/llvm/test/CodeGen/RISCV/VentusGPGPU/`
  - 覆盖 promote-alloca、printf pass、calling convention、join、resource usage 等方向（从文件名可快速判断覆盖点）。
- Clang 用例：
  - `ventus-env/llvm/clang/test/CodeGen/Ventus/`
  - `ventus-env/llvm/clang/test/CodeGenOpenCL/`

---

## 6. 构建入口（本仓库内）

- 一键脚本：`ventus-env/llvm/build-ventus.sh`（构建 LLVM/clang/lld/libclc、并可联动构建 driver/spike/pocl 等外部仓库）
- 使用说明：`ventus-env/llvm/README.md`
