# Ventus ISA -> PTX 静态二进制翻译：可行性与难点分析（按困难度排序）

本文只做分析，不涉及实现。

分析范围以 `VentusInst_basic.txt` 中当前阶段关注的指令为主；若与 `doc/ventus-isa/*` 的约束冲突，以 `doc/` 为语义基准。

必要时可以在 `ventus-env/rodinia/opencl/*/*.dump` 查看实际 Ventus 程序的反汇编做参考

## 当前阶段的前提/输入约束（用于界定“难点边界”）

- 执行模型：Ventus “向量”语义用于表达 SIMT；不使用 RVV mask 机制，`vm` 字段可视为无效，mask 由 SIMT stack 管理。
- SIMT stack：深度固定为 32（warp_size）。
- CSR：除 `CSR_RPC` 会被 `setrpc` 修改外，其它 CSR 在原型阶段视为只读且无副作用。
- 地址空间：通过数值地址区间区分 shared / 指令+静态数据 / global（不使用显式 constant 段）。
- `barrier`：相当于 CUDA `__syncthreads()`；并且要求不能在发散路径上执行。
- corner case：除零、浮点误差等暂不作为阻塞点；访存对齐先按“必须对齐粒度”处理。

上述约束决定了：当前阶段 SBT 的主要难点来自“控制流/地址空间/ABI 状态的等价保持”，而不是常规算术指令本身。

注意：
* 静态翻译后的程序在GPU上运行，难以做任何动态处理

## 困难点与解决方案

除去下边子标题列举的，还有：
* 嵌入到指令中的数据不应当翻译。是否能正确识别到它？如果从ELF中难以识别，是否能从ventus汇编中识别？
  * 目前可认为ventus编译器不会形成这种状况，原型阶段可不管
* 缓存操作/缓存一致性/原子等（原型阶段暂时不管）

### 0) 函数调用

方案见 [function_call.md](./function_call.md)

### 1) SIMT 分支/收敛（`setrpc` + `vbranch` + `join`）

这是决定 SBT 是否能跑通真实 kernel 的头号难点。

- Ventus 把分歧/收敛显式化为：`CSR_RPC` + SIMT stack + current mask。
- `join` 的语义依赖“当前 PC 是否等于栈顶 RPC”，可能是 no-op；并且存在“嵌套分支共享同一 join 汇聚点”导致连续弹栈的情况。
- PTX 的硬件收敛机制没有指令级接口去表达“按 RPC 条件弹栈、恢复 mask、跳转 NewPC”这类软件可见行为。

如果不把 SIMT stack 语义当作一等公民来处理（无论是CFG结构化还原，还是软件化模拟），仅靠直译 `@p bra` 的方式很容易在收敛点选择与 mask 恢复上偏离 Ventus 语义。

方案：[PTX原生方案](./simtstack_hardware.md) 与 [软件模拟方案](./simtstack_software.md)
* PTX原生方案中提到的约束是满足的，优先使用原生方案
  * 目标：对上层软件来说功能相同即可，并不需要还原 ventus simt stack 的压栈/弹栈方案
  * ventus规定的path1/path2执行先后的问题可以不管，只要最终效果相同即可
  * `setrpc` 的数值来源通常为 `auipc` 运算后得到的 PC-relative 地址，需要静态求值
* 补充：若采用“硬件分歧/收敛”方案，需要额外处理 Ventus 标量（warp-uniform）语义在 PTX 上的落地，否则可能出现“发散路径只更新了部分 lane 的标量寄存器值”导致后续 PATH 切换出错。方案见 [scalar_uniform_ptx.md](./scalar_uniform_ptx.md)

### 2) 统一数值地址到 PTX address space 的映射

这是“能否正确访存”的关键难点，通常比单条 load/store 的翻译更本质。

- Ventus 指针是统一的数值地址，地址区间隐含了 shared/global/静态数据等空间。
- PTX 对 shared/global/local/const 的区分是显式的，且指针类型/转换并非完全透明互换。

讨论：SBT 必须能够在 PTX 侧根据地址值把访问路由到正确的 address space，并处理指令/静态数据区间（`0x8000_0000`~）对应的数据承载与重定位问题，否则会出现“翻译后地址计算正确但落在错误空间”的系统性错误。

注意：做SBT不需要将Ventus侧的地址也原封不动地搬过去，我们只需要将ventus shared访存行为映射到ptx shared访存行为，将ventus global访存行为映射到ptx global访存行为。

方案：见 [address_space.md](./address_space.md)

决策：使用方案B
* 方案A性能更好，但有些难以克服的障碍。在原型阶段先采用更稳的方案B

### 3) 控制流与 PC 语义（`auipc/jal/jalr` 及 PC-relative 数据访问）

- SBT 后的“执行地址”不再是原始线性 PC；但原始代码可能通过 `auipc`/PC-relative 方式构造地址（常量池、跳转表、静态数据等）。
- 这要求翻译器掌握原始指令布局/段基址/对齐等信息，并把“原始 PC 空间的引用”改写为 PTX 中的符号/地址。

讨论：只要出现 PC-relative 的数据寻址或间接跳转，SBT 就必须做跨地址空间的系统映射；这类问题往往决定“能否从玩具例子走向真实编译器输出”。

现状与风险分析：
* 分析已有ventus测例的objdump发现jalr指令仅有 `_start` 中手写的一个，在初期阶段可以暂时忽略最麻烦的间接跳转
* 除去 `jalr` 之外的其他跳转/分支 `jal`/`beq`/`vbeq` 都是 PC 相对寻址，不会阻碍静态二进制翻译 
* `auipc` 用于很多计算 PC-relative 的地址，例如用于 `setrpc`/`lw`/`sw`。在初期阶段可不考虑ELF的重定位问题，静态分析这些计算流程应该可行（TODO）

### 4) device ABI 状态：CSR + metadata buffer + spill 基址

- kernel 元信息与运行时状态通过 CSR 暴露（例如 `CSR_KNL`、`CSR_TID/NUMT/WID`、`CSR_LDS/CSR_PDS` 等）。
- 虽然多数 CSR 可视为只读，但它们影响线程/warp/workgroup 的标识计算、参数取值、shared/private 基址、以及（潜在的）spill 布局。

CSR 与内存视图的绑定必须被完整表达；否则即使控制流/算术都正确，也会在参数读取、thread id 计算、shared/private 地址计算等处整体跑偏。

设想：像TID这样的CSR完全可用PTX内嵌的threadId之类的功能取代，LDS/PDS/KNL可作为ptx kernel param传递进来并使用普通PTX寄存器存储

### 5) `barrier` 的合法性约束与等价表达

- 当前阶段把 `barrier` 等同于 CUDA `__syncthreads()`，降低了内存模型映射难度。
- 但仍有一个“硬约束”：不能在发散路径上执行 `barrier`；否则无论 Ventus 还是 PTX，都会引入死锁/未定义行为风险。

`barrier` 的难点不在于把它翻译成某条同步指令，而在于保证它在控制流上处于一致可达位置（否则需要对输入程序/编译器输出施加约束）。

现状：Ventus程序能满足上述条件

### 6) `regext`/`regexti`：前缀解码与语义缺口

- `regext` 作为前缀只作用于下一条指令，要求翻译器以“捆绑的大指令”为单位解码。
- `regexti` 的精确定义若缺失：一旦输入程序使用它，SBT 会在语义基准不完整的情况下无法保证正确。

`regext` 是工程可控的解码复杂度；`regexti` 则可能成为“出现即阻塞”的规格缺口。

现状：现有的Ventus程序完全不存在regexti指令，初期阶段忽略它

### 7) 访存细节（对齐/扩展/谓词化 inactive lane）

- `vl*12.v/vs*12.v` 等向量访存把每 lane 地址/宽度/符号扩展带入翻译。
- 当前阶段把“必须对齐粒度”作为输入约束可降低复杂度，但 inactive lane 是否应完全不发出访存，仍与 mask/SIMT stack 的实现方式强绑定。

结论：在 SIMT/mask 语义明确后，访存多数是可机械翻译的；真正的坑往往来自地址空间映射与谓词化一致性。

### 8) 浮点数学与精度一致性（`vfexp` 等）

- PTX/硬件与库函数的精度、近似、NaN/denorm 处理可能与 Ventus 期望不完全一致。
- 若目标允许“数值近似但功能正确”，难度可控；若追求位级一致，难度会显著上升。

初期阶段不考虑这些难点，浮点数完全依赖NV硬件即可

### 9) 除法/取余与 corner case（`div/rem` 等）

- 除 0、溢出边界等在严格规范下需要精确定义。
- 在“原型验证允许忽略 corner case”的前提下，这通常不是阻塞点。

## 对 `VentusInst_basic.txt` 的使用建议（仅从分析角度）

- 将其视为“指令覆盖范围清单”；涉及 SIMT/mask/CSR/地址空间/`barrier` 的行为，以 `doc/ventus-isa/*` 的约束为准。
- 特别是：`vbranch/join/setrpc` 在 `VentusInst_basic.txt` 的简化描述不足以作为 SBT 语义基准。

## 建议的验证闭环（仍只做分析，不涉及实现）

1. 先验证 SIMT stack + mask：`setrpc + vbranch + join + endprg` + 少量 `V` 算术。
2. 再验证地址空间路由：shared 区间与 global 区间的读写（配合 `vlw12.v/vsw12.v`）。
3. 再验证 ABI：`zicsr` 读取 `CSR_KNL/CSR_TID/CSR_NUMT/...` 的端到端正确性。
4. 最后引入 `barrier`（确保在一致控制流位置），以及逐步收紧 corner case 语义。
