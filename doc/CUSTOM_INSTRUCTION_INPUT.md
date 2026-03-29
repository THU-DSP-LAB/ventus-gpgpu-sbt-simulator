# Custom Instruction Input

> Status: `active`
>
> Role: tracked input material for the active custom-instruction planning work.
>
> This document is not a `current` contract. It is an input source for `doc/CUSTOM_INSTRUCTION_SHARED_BASELINE.md` and the active OpenSpec changes. If its contents conflict with current specs or synced change artifacts, the current/spec-synced artifacts win.

Below is the migrated input content from the former repo-root `custom-instruction-definition.md`.

# custom 指令定义

## Warp-level 指令

### 1.1 MMA（Tensor Core）

- **执行粒度**：warp-level（32 线程协同完成一次 MMA）
- **寄存器绑定**：指令编码起始寄存器号，隐含连续寄存器窗口；32-bit 版本 C/D 复用同一窗口（结果覆盖输入 C）
- **汇编格式**
  - 32-bit：
  ```asm
  mma.shape.alayout.blayout.dtype.atype.btype.ctype d, a, b
  ```

- **shape**：`m8n8k16 / m16n8k16 / m8n16k16 / m16n16k16/ m8n8k8 / m16n8k8 / m8n16k8 / m16n16k8`
- **layout 约定**
  - A：`0=row, 1=col`
  - B：`1=row, 0=col`
- **精度组合**
  - `f32.tf32.tf32.f32`
  - `f32.f16.f16.f32`
  - `f16.f16.f16.f16`
  - `f32.bf16.bf16.f32`
#### MMA 编码规划（32-bit 指令字）

字段位置与含义（与现有 MMA 32-bit 格式一致）：

```
[31:28] abtype   A/B 类型
[27:25] shape    m×n×k 形状
[24:20] vrs2     B 起始向量寄存器
[19:15] vrs1     A 起始向量寄存器
[14]    alayout  A layout（0=row,1=col）
[13]    blayout  B layout（1=row,0=col）
[12]    cdtype   C/D 类型（0=fp16,1=fp32）
[11:7]  vrd      C/D 起始向量寄存器（32-bit 版本 C 与 D 复用）
[6:0]   opcode   MMA 主 opcode = 0b0001010
```

枚举规划：

- `shape`：
  - `000` m8n8k16
  - `001` m16n8k16
  - `010` m8n16k16
  - `011` m16n16k16
  - `100` m8n8k8
  - `101` m16n8k8
  - `110` m8n16k8
  - `111` m16n16k8
- `abtype / cdtype`（用于区分三类精度组合）：
- abtype 编码：
  - 0 = TF32
  - 1 = FP16
  - 2 = BF16
- cdtype 编码：
  - 0 = fp16
  - 1 = fp32
- 组合约束：
  - `f32.tf32.tf32.f32`：abtype=TF32，cdtype=1，仅允许 `k8` 形状
  - `f32.f16.f16.f32`：abtype=FP16，cdtype=1
  - `f16.f16.f16.f16`：abtype=FP16，cdtype=0
  - `f32.bf16.bf16.f32`：abtype=BF16，cdtype=1
- TF32 额外约束：
  - 仅支持 `TF32 x TF32 -> FP32 accumulate`
  - 不支持 `FP16 accumulate`
  - 寄存器窗口大小、row/col layout 语义与现有 MMA 保持一致
  - 与现有 `k16` 家族唯一的形状差异是 `K = 8`
### 2.1 Shuffle

- **执行粒度**：warp-level（warp 内 lane 间数据交换）
- **指令集合（RVV 风格）**
```
shuffle.idx   vd, vs2, lane
shuffle.up    vd, vs2, offset
shuffle.down  vd, vs2, offset
shuffle.bfly  vd, vs2, mask
```

#### 汇编格式说明

- `vd`：目的向量寄存器
- `vs2`：源向量寄存器
- `lane/offset/mask`：5-bit 立即数参数（0..31）
- `vm/m`：沿用 OP-V 同构编码中的该比特位，但**当前 Ventus ISA 不使用 RVV mask 语义**
  - `v0` 是普通向量寄存器，不是 mask 寄存器
  - 该位在当前语义下不触发 `v0`-as-mask 行为；如后续需要赋予额外语义，必须先更新 canonical spec
#### 指令编码规划（32-bit 指令字，OP-V 同构）

统一归为同一 opcode 家族，字段如下：

```
[31:26] funct6   shuffle 子操作选择
[25]    m        mask 位（vm）
[24:20] vs2      源向量寄存器
[19:15] imm5     lane/offset/mask（5-bit 立即数）
[14:12] funct3   固定 = 001
[11:7]  vd       目的向量寄存器
[6:0]   opcode   SHFL 主 opcode = 0b1000010
```

`funct6` 枚举：

- `001001` shuffle.idx
- `001010` shuffle.up
- `001011` shuffle.down
- `001000` shuffle.bfly

## 向量指令（RVV 风格）

以下指令均以“向量寄存器 + vtype/vl”语义执行：输入输出为 v0..v31，并服从 vl 元素数约束。`vtype` 中 `vsew/vlmul` 控制元素宽度与寄存器分组；当前 Ventus ISA 不使用 RVV `v0` mask 机制，mask 由 SIMT stack 管理。

为避免与标准 OP-V（opcode=0x57）已有算术/配置指令编码空间产生耦合，向量扩展统一放在本项目规划的 `..10` opcode 空间（0x0A/0x2A/0x5A/0x7A；shuffle 使用 0x42），但字段排布保持 OP-V 同构（funct6/vm/vs2/vs1/funct3/vd/opcode），便于复用 RVV 解码/寄存器读写通路；其中 `vm`/`m` 位当前仅表示编码占位，不引入独立 mask 语义。

向量同构编码的字段位置（与 OP-V 类似）：funct6|vm|vs2|vs1|funct3|vd|opcode，其中 `vm` 位当前不触发 `v0` mask 语义。 

### 2.1 向量 Convert（v-cvt）

- **指令集合**
```asm
vcvt.f32.fp16    vd, vs
vcvt.f16.fp32    vd, vs

vcvt.fp32.bf16   vd, vs
vcvt.bf16.fp32   vd, vs
```

- **语义**：逐元素转换；舍入模式固定 rn（后续可扩展）
#### v-cvt 编码规划

主 opcode：**CUSTOM-0 = 0x0B**

```
[31:26] funct6  转换种类
[25]    vm      OP-V 同构编码位；当前 Ventus 语义下不启用独立 mask
[24:20] vs2     源向量寄存器（vs）
[19:15] vs1     预留（置 0）
[14:12] funct3  预留/版本（置 0）
[11:7]  vd      目的向量寄存器
[6:0]   opcode  0x0B
```

`funct6` 枚举：

- `000000` vf32 ← vf16
- `000001` vf16 ← vf32
- `000010` vf32 ← vbf16
- `000011` vbf16 ← vf32
### 2.2 向量 CUDA core packed（v-f16x2 / v-bf16x2）

“x2”解释为：每个 32-bit lane 内包含两个 16-bit 元素（packed lanes）。该类指令按 32-bit 容器逐元素执行，每个容器内部做两路 16-bit 并行。

- **指令集合**
#### FP16x2 packed

```asm
vadd.f16x2   vd, vs1, vs2
vmul.f16x2   vd, vs1, vs2
vfma.f16x2   vd, vs1, vs2    ; vd = vs1*vs2 + vd
```

#### BF16x2 packed

```asm
vadd.bf16x2  vd, vs1, vs2
vmul.bf16x2  vd, vs1, vs2
vfma.bf16x2  vd, vs1, vs2    ; vd = vs1*vs2 + vd
```

#### packed 编码规划

主 opcode：**CUSTOM-2 = 0x5B**

```
[31:26] funct6  操作(add/mul/fma)
[25]    vm      OP-V 同构编码位；当前 Ventus 语义下不启用独立 mask
[24:20] vs2
[19:15] vs1
[14:12] funct3  数据类型选择
[11:7]  vd
[6:0]   opcode  0x5B
```

`funct3`（dtype）枚举：

- `000` f16x2
- `001` bf16x2
`funct6`（op）枚举：

- `000000` add
- `000001` mul
- `000010` fma（累加到 vd：`vd = vs1*vs2 + vd`）
### 2.3 向量 SFU（approx）

- **FP32 向量**
```asm
vex2.approx.f32   vd, vs
vlg2.approx.f32   vd, vs
vrcp.approx.f32   vd, vs
vsqrt.approx.f32  vd, vs
vrsqrt.approx.f32 vd, vs
vsin.approx.f32   vd, vs
vcos.approx.f32   vd, vs
vtanh.approx.f32  vd, vs
vgelu.approx.f32  vd, vs
vsilu.approx.f32  vd, vs
```

- **FP16x2 packed 向量**
```asm
vex2.approx.f16x2   vd, vs
vrcp.approx.f16x2   vd, vs
vsqrt.approx.f16x2  vd, vs
vrsqrt.approx.f16x2 vd, vs
vtanh.approx.f16x2  vd, vs
vgelu.approx.f16x2  vd, vs
vsilu.approx.f16x2  vd, vs
```

- **BF16x2 packed 向量**
```asm
vex2.approx.bf16x2   vd, vs
vrcp.approx.bf16x2   vd, vs
vsqrt.approx.bf16x2  vd, vs
vrsqrt.approx.bf16x2 vd, vs
vtanh.approx.bf16x2  vd, vs
vgelu.approx.bf16x2  vd, vs
vsilu.approx.bf16x2  vd, vs
```

#### SFU 编码规划

主 opcode：**CUSTOM-1 = 0x2B**

```
[31:26] funct6  SFU 函数选择
[25]    vm      OP-V 同构编码位；当前 Ventus 语义下不启用独立 mask
[24:20] vs2     源向量寄存器（vs）
[19:15] vs1     预留（置 0）
[14:12] funct3  数据类型选择
[11:7]  vd
[6:0]   opcode  0x2B
```

`funct3`（dtype）枚举：

- `000` fp32
- `001` f16x2
- `010` bf16x2
`funct6`（function）枚举：

- `000000` ex2
- `000001` lg2
- `000010` rcp
- `000011` sqrt
- `000100` rsqrt
- `000101` sin
- `000110` cos
- `000111` tanh
- `001000` gelu
- `001001` silu

## 编译器工作

- [x] 汇编/反汇编
- [x] LLVM 前端
- [x] bulit-in function  

## ~~pending ->~~使用 shuffle 指令完成

- [ ] ~~wrap.sum~~
- [ ] ~~wrap.max~~
