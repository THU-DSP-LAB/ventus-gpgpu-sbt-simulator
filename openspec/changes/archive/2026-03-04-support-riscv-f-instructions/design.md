## Context

`VentusInst_basic.txt` 增加了一批标量单精度浮点相关 mnemonic（`fadd.s/fmul.s/flw/...`）。本项目当前的实现边界是：

- 前端解码：优先用 Spike pattern 命中 Ventus/RVV 指令；未命中则走 `decode_scalar()`，目前覆盖 RV32I/M + CSR，且仅把少量标量浮点 move 当作“可忽略 padding/NOP”；
- 后端 PTX：对标量整数/内存与向量指令已有一套 fail-fast lowering；对新增标量浮点 mnemonic 缺失实现会触发 `unknown/unsupported`；
- 覆盖 gate：`tools/microtest_coverage_gate.sh --require-full` 以 `VentusInst_basic.txt` 为目标集合（减去 `data/inst_exceptions.txt`）做强约束。

Ventus 的标量 ISA 在项目上下文里被描述为接近 `RV32IMA_zicsr_zfinx`（见 `openspec/project.md`），因此本变更采用 **Zfinx 模型**：标量浮点值以 `f32` raw bits 形式承载在 **X 寄存器**（`x0..x31`）中，不引入独立的 FPR regfile。

## Goals / Non-Goals

**Goals:**
- 使 `VentusInst_basic.txt` 新增的标量浮点 mnemonic 达到 compile-supported：`sbt_decode --require-known` + `sbt_ptx --require-known` + `ptxas` 编译通过。
- 为这些指令补齐 Spike-vs-PTX microtest，对关键语义做回归对齐（浮点用 atol/rtol；整数结果精确匹配）。
- 与现有 “scalar uniform datapath” 设计对齐：复用 `scalar_exec_leader_only` 与 warp 同步语义，不引入新的运行期兜底/模拟路径。

**Non-Goals:**
- 不实现/不建模 `frm/fflags`（FCSR）完整语义与异常标志传播。
- 不覆盖 `D` 扩展（双精度）或更广泛的浮点扩展（例如 `Zfa/Zfh` 等）。
- 不引入新的“列出缺失指令清单”的诊断子命令；保持现有 fail-fast 风格。

## Decisions

### 1. Zfinx Register Model: float-in-X, no new regfile

**Decision:** 将标量 `F` 类指令视作在 **X 寄存器**上操作：`xN` 中存放 `f32` 的 bit pattern（u32），ALU 计算时在 PTX 中用 `.f32` 临时寄存器做运算，最后以 `mov.b32` 写回到整数域并存回 `WarpCtx` 的 x-reg 槽位。

**Rationale:** 项目背景已明确 Ventus 标量部分接近 `zfinx`；现有实现也只有 `RegClass::{X,V}`，且标量路径（WarpCtx/shared）已成熟。引入独立 FPR 会扩大 IR、存储布局与 emitter 复杂度，并与现有 microtest/覆盖工具的“mnemonic-by-name”模型产生额外耦合。

**Alternatives considered:**
- 引入 `RegClass::F` + 独立 FPR 存储：Rejected，改动面大且与 Ventus `zfinx` 预期不匹配。
- 把浮点指令当作 NOP：Rejected，会直接破坏覆盖 gate 且语义不可用。

### 2. Decode: name normalization + explicit rounding-mode capture

**Decision:** 解码阶段对新增标量浮点指令采用：
- `VentusInst_basic.txt` 对齐的 **名字规范化**：以 `_` 代替 `.`（如 `fadd.s` → `fadd_s`）；
- 对带 `rm` 字段的指令（`fadd/fsub/fmul/fdiv/fsqrt/fmadd/.../fcvt.*`）显式解析 `rm`，并在 `DecodedInst` 中携带一个 `fp_rm` 字段供 lowering 使用。

**Rationale:** `fcvt.w.s` 等指令在真实编译产物中常依赖 `rm=RTZ`（符合 C cast toward-zero 需求）。如果解码阶段丢失 `rm`，后端将无法做到“无声但正确”，容易变成隐性语义漂移。`rm` 不应编码进 mnemonic 名字（否则覆盖工具与目标表会漂移），因此需要独立字段承载。

**Alternatives considered:**
- 忽略 `rm`，统一按 RNE lowering：Rejected，`fcvt.*` 语义高风险。
- 将 `rm` 变体编码进 `di.name`：Rejected，会破坏 `VentusInst_basic.txt` 的 mnemonic 统计与覆盖对齐。

**Rounding policy (prototype):**
- 支持 `RNE/RTZ/RDN/RUP` 映射到 PTX 的 `.rn/.rz/.rm/.rp`；
- `DYN (rm=111)` 在不建模 `frm` 的前提下视作 `RNE`（文档明确该假设）；
- `RMM` 若 PTX 无直接对应，将明确 fail-fast（错误信息包含 rm 值与 pc）。

### 3. Lowering strategy: reuse scalar uniform datapath + bitcast via `mov.b32`

**Decision:** 标量浮点 lowering 复用现有标量辅助函数与 leader-only 机制：
- 使用 `emit_ld_x_u32_scalar/emit_st_x_u32_scalar` 读写 x-reg（WarpCtx/shared），并保持 `scalar_prefix()` predication 语义；
- 使用 `mov.b32` 在 `%r`（u32 bits）与 `%f`（f32）之间转换表示；
- 对 `feq_s/flt_s/fle_s/fclass_s` 等产生整数结果的指令，按 0/1 写回 x-reg，匹配 Spike 结果并便于 host 对比。

**Alternatives considered:**
- 为浮点单独维护一套“per-lane 标量”执行模型：Rejected，与当前标量 uniform 约定冲突且引入大量重复逻辑。

### 4. Memory ops: `flw/fsw` treated as u32 load/store with float mnemonic

**Decision:** `flw/fsw` 在 Zfinx 模型下本质是 “load/store 32-bit bits”。实现上：
- 解码：识别 opcode `0x07/0x27` 并产出 `di.name = flw/fsw`（而不是复用 `lw/sw` 名字）；
- lowering：复用标量地址映射与 `ld/st.u32` 路径，实现与 `lw/sw` 等价的内存行为，但保留 mnemonic 名字以满足覆盖统计。

**Alternatives considered:**
- 在解码阶段把 `flw/fsw` 直接重命名为 `lw/sw`：Rejected，会导致 `VentusInst_basic.txt` 的目标 mnemonic 永远无法被覆盖统计命中。

### 5. Per-instruction-family semantics mapping

**Decision:** 关键指令族按如下方式映射（均在 scalar uniform 路径内）：
- ALU (`fadd_s/fsub_s/fmul_s/fdiv_s/fsqrt_s`): `mov.b32` → PTX `add/sub/mul/div/sqrt.<rm>.f32` → `mov.b32` 回写。
- FMA family:
  - `fmadd_s`: `fma.<rm>.f32 a,b,c`
  - `fmsub_s`: `fma.<rm>.f32 a,b,-c`
  - `fnmsub_s`: `fma.<rm>.f32 -a,b,c`（或等价变体）
  - `fnmadd_s`: `fma.<rm>.f32 -a,b,-c`（或等价变体）
- Sign injection (`fsgnj_*`): 在整数域用 bit-mask 操作 sign bit（`0x80000000`) 与 magnitude（`0x7fffffff`）。
- Min/Max (`fmin_s/fmax_s`): 显式处理 NaN 规则（单 NaN 返回非 NaN，双 NaN 返回 canonical NaN），并尽量对齐 `-0/+0` 的选择规则；实现上可参考现有 NaN-safe `fmax` builtin 的 predicated 结构。
- Compare (`feq_s/flt_s/fle_s`): 用 PTX `setp.*.f32` + `selp.u32` 产出 0/1；NaN 情况按 Spike/RISC-V 规则产生 false。
- Conversions:
  - `fcvt_s_w/fcvt_s_wu`: `cvt.<rm>.f32.s32/u32`
  - `fcvt_w_s/fcvt_wu_s`: `cvt.<rm-int>.s32/u32.f32`（选择与 `rm` 对应的 PTX rounding variant）
- `fclass_s`: 参考现有 `vfclass_v`，按 IEEE-754 bit layout 计算 10-bit class mask（区分 sNaN/qNaN）。
- `fmv_w_x/fmv_x_w`: bitwise move（RV32 下等价于 `mv`），确保保持 bits 不变。

## Risks / Trade-offs

- **浮点细节差异风险**：GPU/PTX 的实现可能涉及 FTZ/DAZ、`div/sqrt` 精度、NaN payload 传播等差异；microtest 需要覆盖常见路径并使用容差对比，必要时对 `fmin/fmax/fclass` 做更严格的 bit 级对齐。
- **`rm=DYN` 假设**：不建模 `frm` 意味着 `rm=111` 将被当作 RNE；如果真实程序修改 `frm`，Spike-vs-PTX 可能产生系统性偏差，需要在后续评估是否值得引入最小 `frm` 建模。
- **转换溢出/NaN 行为**：`fcvt.w*.s` 的 out-of-range/NaN 行为在不同后端上可能需要额外对齐；初期 microtest 应避免未定义/实现相关区间，后续再逐步加严。
- **覆盖 gate 压力**：一旦 `VentusInst_basic.txt` 扩展，microtest 覆盖不足会直接 fail-fast；需要同时推进“实现 + microtest 触达 + 覆盖统计”三者一致。

