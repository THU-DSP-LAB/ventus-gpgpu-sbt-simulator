# Ventus 统一数值地址 → PTX Address Space：静态翻译方案

## 1. 背景：问题到底是什么

Ventus ISA 侧把指针当作**统一的数值地址**使用，并用地址区间隐含地区分地址空间（shared / 指令+静态数据 / global）。而 PTX 的 `.shared/.global/.local/.const/.param` 是**显式状态空间**，并且不少指令在语法层面就要求指定空间（或要求输入是“可落入某个窗口”的 generic 地址）。

静态二进制翻译（SBT）要解决的不是“某条 `vlw12` 翻成哪条 `ld`”，而是：**翻译后的“地址值”必须能在 PTX 侧落入正确的地址空间，并被正确路由。**

Ventus的部分现状：
* shared memory 的用量如同nvidia一致：每个kernel的用量不同，且每次线程块新下发会为它新分配shared memory可用的基地址，生命周期等同于线程块生命周期。目前总可以假定Ventus kernel的shared memory用量少于NVIDIA GPU上每个SM的shared memory资源量，因此总可以将Ventus kernel的shared memory映射到NVIDIA shared memory。程序若访问超出自己声明量的shared memory则其行为undefined
* 本静态二进制翻译仿真器可以认为是“用户态”仿真：只包含一个GPGPU虚拟地址空间

## 2. Ventus 的地址空间约束（语义基准）

来自 `doc/ventus-isa/others.md`：

- `0x7000_0000` ~ `0x7fff_ffff`：shared memory（每个 thread block 基地址不同，必须从 `CSR_LDS` 获取）
- `0x8000_0000` ~ `0x8fff_ffff`：指令与部分编译器嵌入的静态数据
- 其他：global memory（Ventus 没有显式 constant 段；CUDA local memory 视为 global 上的一段）

## 3. PTX 是否支持 “generic 地址自动路由到 shared/global”？

支持，但前提是：你提供的地址必须是 **PTX 定义的 generic address**（通常来自 `cvta`，或来自某个 state space 变量的地址），而不是随便一个整数常量。

PTX ISA（Release 8.4）对 *Generic Addressing* 的描述要点：

- **如果内存指令不指定 state space，则使用 Generic Addressing**（例如 `ld.b32` / `st.b32`，而不是 `ld.global.b32` / `st.shared.b32`）。
- `.const/.param/.local/.shared` 在 generic 地址空间中被建模成若干“窗口”（window base + window size）。
- 一个 generic 地址**默认映射到 global**；只有当它落入 `.const/.local/.shared` 等窗口时，才会映射到对应空间。
- `isspacep` 用于判断一个 generic 地址是否落入某个窗口；`cvta` 用于在 generic 与 `.const/.global/.local/.shared/.param` 之间转换，且当地址不在窗口内时转换结果是 *undefined*，因此可用 `isspacep` 保护。

结论：**PTX 的确存在“统一（generic）地址 + 自动路由”的能力**；SBT 只需要确保生成的地址值在 PTX 的意义上是有效的 generic 地址，并且落入正确窗口即可。

## 4. 方案集合：Ventus 统一地址如何翻成 PTX

下面列出三类可选方案，按“实现难度/鲁棒性/性能”排序。当前阶段（验证 `vlw12/vsw12` 等普通访存）推荐先做 4.1。

### 4.1 方案 A（推荐）：**指针重写** + **generic `ld/st`**

核心思想：翻译后不再保留 Ventus 的“虚拟数值地址”，而是把 Ventus 程序中所有“指针值”直接重写为**真实的 PTX 指针（generic address）**。这样：

- shared 访问：指针值天然落入 `.shared` 窗口
- global 访问：指针值落不到任何窗口时，generic 路由默认走 global
- 翻译 `vlw12/vsw12` 时可以统一使用 `ld.*` / `st.*`（不写 `.shared/.global`），由 PTX generic addressing 自动路由

这正是 `lab/03_sbt_feasibility/README.md` 中“只需要保持 shared/global 行为等价；基地址可替换为 PTX/CUDA 原生分配地址，偏移不动”的具体化落地。

#### 4.1.1 CSR / 基地址的映射

- `CSR_LDS`（Ventus shared 基址）：
  - 在 PTX 中用 dynamic shared 或静态 `.shared` 变量承载 shared。
  - 用 `cvta.shared` 取得该 `.shared` 变量的 **generic 地址**，作为翻译后 `CSR_LDS` 的值。
  - 之后 Ventus 程序对 `CSR_LDS + offset` 的所有计算，都会自然地产生落在 shared 窗口内的 generic 地址。

- global 相关基址（例如 `CSR_KNL` 指向的 metadata buffer、kernel 参数区、堆等）：
  - 通过 PTX kernel param 传入（host 侧分配得到的 global 指针本身就可作为 generic 地址使用）。

- `0x8000_0000`（指令+静态数据区）：
  - 若当前阶段不涉及 PC-relative 常量池/跳转表，可先不实现该区的“数值地址兼容”；
  - 一旦需要支持绝对/PC-relative 访存，建议把静态数据放在 `.const` 或 `.global` 符号中，并在翻译时把“落在 0x8000_0000~ 的地址常量”重定位为对应符号的地址（同样用 `cvta.const`/`cvta.global` 取得 generic 地址作为基址）。

#### 4.1.2 统一指针表示（翻译器内部约定）

- Ventus 是 RV32 语境下的 32-bit 指针；翻译到 PTX 时统一使用 `.u64` 的 generic pointer 表示地址寄存器。
- 所有“会被当作地址用”的值（来自 `CSR_LDS` / kernel params / 由它们参与运算得到的结果）都保持为 `.u64`，避免频繁扩展/截断。

#### 4.1.3 访存翻译模板（以普通 load/store 为例）

- Ventus：`vlw12.v` / `vsw12.v`（每 lane 一个地址）
- PTX：对每个 lane 的地址寄存器 `%rd_addr` 直接发出 `ld.*` / `st.*`（不写 state space），例如：
  - `ld.u32 %rX, [%rd_addr];`
  - `st.u32 [%rd_addr], %rY;`

由 PTX generic addressing：

- 如果 `%rd_addr` 落入 shared 窗口 → 等效为 shared load/store
- 否则 → 等效为 global load/store

#### 4.1.4 该方案的关键假设与风险

- 关键假设：Ventus 程序不会依赖“地址数值”做逻辑
  - 例如：通过比较 `addr & 0xF000_0000` 来判断地址空间，或者任何与地址数值相关的比较/哈希/序列化
  - 因为方案 A 会把指针值替换成 PTX 指针，数值分布不再保留 `0x7000_0000/0x8000_0000` 这些区间特征。
  - 对一般编译器输出的 kernel，这是合理假设：区间判定属于 ABI/实现细节，不应出现在设备端通用代码中。
- 风险：硬编码、指针逃逸
  - 在ventus程序中硬编码的地址（绝对地址或PC-relative）需要一个不落地替换成ptx地址
    - 大部分基地址都是由runtime生成并以 kernel param / CSR 形式传递，这部分地址runtime/driver知晓其语义，容易替换成ptx地址
    - Ventus ELF中含有静态数据段，以及难以知晓是否有指令-数据混合的情况，这部分地址会被硬编码到程序中且难以做语义识别（一个硬编码的0x80001000如何知晓其是指向静态数据段的指针，还是单纯的程序数据？）
    - 指针逃逸：指针可能存储于上述静态数据段/混合到指令段中的数据中，更加难以识别
    - 现有Ventus编译器/软件栈/测例大概除了ELF静态数据段不会硬编码其他地址，但若使用此方案是否会对项目脱离原型期后的发展造成困难？
- 实际困难：指针宽度
  - 现代GPU上都是64bit的ptx指针，而ventus上暂无64bit运算，如果预先将所有ventus地址全部改为ptx generic address就必须确保指针运算全部翻译成64bit，但如何区分一个运算语义上是否在操作指针可能比较困难（考虑到指针逃逸：指针先存储到内存，稍后再读回，甚至多层间接寻址），可能的方案：
    - 将所有整数运算全部改为64bit（乘除法开销更大），访存则更加艰难（ventus程序只给32bit存储指针，存不下64bit ptx指针）
    - 将ptx指针改回32bit（近些代NVIDIA GPU在64bit host上似乎已经不保证此正确性了）

### 4.2 方案 B：保留 Ventus 数值地址，**按区间显式分流**

如果必须保留 Ventus 地址数值语义（例如设备代码确实会用区间判断）
* 申请大块的nvidia显存buffer作为ventus global显存，保存其基地址为 64bit ptx 指针 `ventus_global_base`
  * 例如 2GiB 以覆盖Ventus地址段0x8000_0000 ~ 0xFFFFFFFF，NVIDIA显存充足的话应该不是问题
  * Ventus 地址段 0x7000_0000 ~ 0x7FFF_FFFF 是shared memory，映射为 NVIDIA shared memory
  * Ventus 地址段 0x0000_0000 ~ 0x6FFF_FFFF 在当前的Ventus软件栈/驱动程序中几乎不用，本项目目前可忽略它，如果需要的话就改为在NVIDIA GPU上申请单个4GiB buffer以覆盖完整的Ventus 32bit地址空间
* 在每次访存时：
  1. 判断32bit Ventus地址 `addr` 是否在 `0x7000_0000~0x7fff_ffff`（shared）
  2. shared：计算 `offset = addr - CSR_LDS_ventus`，再用 PTX shared base 指针加 offset，发 `ld.shared/st.shared`
  3. 否则：将 32bit ventus address 加 64bit ptx 指针 `ventus_global_base` 成为ptx地址，然后 `ld.global/st.global`
* 可以将 Ventus ELF 数据也加载到上述buffer（0x8000_0000段），这样可以使静态二进制翻译后的程序能正常访问内嵌到ELF中的数据

优点：
* 解决Ventus程序中所有直接/间接硬编码地址的问题，兼容性好
* 维持32bit Ventus地址，所有Ventus整数指令可保持32bit位宽，每次访存前才将Ventus地址与 64bit ptx 指针 `ventus_global_base` 做加法得到 64bit ptx 地址
  * 注意Ventus地址需要视为unsigned，做加法时不能符号扩展

缺点：

- 每次访存都有额外的比较/分支（或 predication）+ 整数加法
  - 这可能与其他SIMT warp diverge嵌套
  - Ventus上通常每条指令要么访问 shared memory 要么访问 global memory，几乎不出现同一条访存指令 warp 中部分thread访问shared部分thread访问global的情况，因此可认为性能开销主要在额外增加的比较，分支开销较少。
- 必须维护 Ventus 的 “CSR_LDS_ventus 数值” 与 “PTX shared base 指针” 两套基址语义

未来演进：
* 可以不一次性申请2GiB/4GiB这样大的单一buffer，而是先用 `cuMemAddressReserve` 保留一段连续的地址空间（例如 4GiB），之后需要用哪些Ventus地址就将保留好的CUDA虚拟地址真实分配出来（`cuMemCreate` + `cuMemMap` + `cuMemSetAccess`）

### 4.3 方案 C：混合（静态推断优先，无法推断再动态分流）

实际工程上常见：

- 对大多数地址：通过数据流分析判断其来源（`CSR_LDS` 派生 → shared；kernel param 派生 → global），直接发 `ld.shared/ld.global`
- 对少数“类型丢失/指针混用”的地址：用 `isspacep` + `cvta.to.shared`/`cvta.to.global` 做动态分流

这在以下场景很有价值：

- 你要翻译的不是简单 `ld/st`，而是一些**不支持 generic 形式**或性能敏感的指令（例如部分 atomic、某些异步拷贝路径等）
- 你希望尽量避免 generic 路由带来的额外开销/不确定性

## 5. 推荐落地路线（面向当前原型阶段）

1. 先实现 **方案 A**：把 `CSR_LDS` 映射到 `.shared` 的 generic base（`cvta.shared`），访存统一用 generic `ld/st`，先跑通 shared/global 的基本读写。
2. 需要支持 `0x8000_0000~` 静态数据访问时，再补“静态数据承载 + 重定位”（把原始地址常量改写为符号地址）。
3. 遇到必须用显式空间的指令（或 generic 不可用/性能不可接受）时，引入 **方案 C** 的 `isspacep/cvta` 分流作为补丁路径。
