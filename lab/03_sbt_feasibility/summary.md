# 原型验证阶段方案总结（Ventus ISA → PTX 静态二进制翻译）

目标：在“能跑通测例优先”的前提下，先跑通 `ventus-env/rodinia/opencl` 的 7 个测例（bring-up 阶段可先只跑 `vecadd`）。原型阶段不做兜底（不实现 software SIMT stack、不实现 PC dispatch 等），遇到超出约束/无法处理的输入直接报错并退出。

语义基准：若 `VentusInst_basic.txt` 与 `doc/ventus-isa/*` 有冲突，以 `doc/` 为准。

---

## 1. 原型阶段需要做的功能 / 不需要做的功能

### 1.1 需要做（最小原型必须具备）

1) **指令解码与最小 IR/CFG**
- 能从 Ventus ELF / `.dump` 获取指令序列与 PC（按 PC 切基本块、构建 CFG）。
- 能识别并拒绝当前原型不支持的控制流形态（见 1.2）。

2) **SIMT 分支/收敛：`setrpc` + `vbranch` + `join`**
- 采用“CFG 结构化还原 + PTX/NVIDIA 硬件分歧/收敛”的方案（见 2.1），将 Ventus 显式 join 点还原为 PTX 的 merge label，并依赖硬件 active mask 得到等价的 lane 生效行为。

3) **函数调用/返回：`jal` / `jalr`**
- 采用“PTX `call/ret` 承载控制转移 + Ventus ABI/寄存器文件作为被模拟状态”的混合方案（见 2.2）。

4) **统一数值地址 → PTX address space 路由（地址空间映射）**
- 采用“方案 B：保留 Ventus 32-bit 数值地址 + 按区间显式分流”（见 2.3），并在 GPU 上申请足够大的 global buffer（2GiB/4GiB）作为 Ventus 全局地址空间承载。

5) **device ABI 状态：CSR 与 kernel 元数据/参数**
- 原型阶段除 `CSR_RPC` 外其它 CSR 视为只读且无副作用；将必要信息用 PTX kernel param 传入或用 PTX 内建语义替代（见 2.4）。

6) **`barrier`**
- 等价翻译为 CUDA `__syncthreads()` / PTX `bar.sync`，并将“不能在发散路径上执行”作为硬约束（见 2.5）。

7) **指令子集（以测例覆盖为准）**
- 覆盖跑通 `rodinia/opencl` 7 个测例所需的最小指令集合：基础标量算术/逻辑/分支，必要的 `V` 算术与 `vlw12.v/vsw12.v` 等访存，`zicsr` 读 CSR，以及 `endprg`。
- 不追求 corner case（除零、浮点误差、NaN/denorm 等）完全一致；以“测例可通过”为验收标准。

### 1.2 不需要做（原型阶段明确不做/不支持）

- **兜底方案不做**：不实现 `simtstack_software.md` 的 software SIMT stack；不实现 `function_call.md` 的 PC 调度器（解释器式 dispatch）；遇到不可结构化 CFG / 非标准返回 / computed goto 等直接报错退出。
- **间接控制流不支持**：`jalr` 不用于 jump table / 任意 computed goto；不支持“ra 被改写导致返回到任意地址”的语义。
- **复杂内存与一致性不做**：原子、缓存操作/一致性、复杂内存模型映射等。
- **指令覆盖不追求全面**：`regexti` 不支持；`regext` 前缀复杂解码若测例不出现可延后；其它未在测例覆盖内的指令直接拒绝。
- **PC-relative/重定位全面正确性不追求**：不做完整 ELF 重定位与“指令混入数据”的通用识别；以现有测例形态为输入约束（需要时通过全局 buffer 装载静态数据段满足访问）。
- **数值严格一致性不做**：浮点严格一致、除法/取余 corner case 等不作为原型阻塞点。
- **性能优化不做**：只要能跑通测例即可。

---

## 2. 需要做的功能：实现方案

### 2.1 SIMT 分支/收敛（选择：PTX 硬件分歧/收敛 + CFG 结构化还原）

目标：把 Ventus 的 `setrpc + vbranch + join` 还原成“常规 if/else CFG + merge label”，并依赖 NVIDIA 硬件 divergence/reconvergence 自动维护 active mask。

**输入约束（原型阶段写死；不满足则报错退出）**
- `CSR_RPC`/join 点必须能静态解析为函数内确定 PC/label：典型模式是 `auipc t1, 0` + `setrpc ..., t1, imm`。
- 对每个 `vbranch`：解析得到的 join 块 `J` 必须是该分支的后支配点（post-dominator），且分支区域近似 SESE（single-entry single-exit），无侧出口/多入口。
- `jalr`/间接跳转等不可结构化控制流不出现在受 `vbranch/join` 约束的区域内。
- `barrier` 只出现在完全收敛处（等价于不在发散路径上执行）。
- 不要求复刻 Ventus “PATH1=线程更少路径先执行”的次序（按 README 约束：顺序不可观测即可）。

**实现要点（管线）**
1) 解码指令、按 PC 切基本块，构建 CFG（显式边：`jal/jalr/branch/vbranch`）。
2) 标量/warp-uniform 常量传播与符号求值：在每条 `vbranch` 处解析“当前 `CSR_RPC` 的 join PC”（至少支持 `auipc` + 加法 + 立即数）。
3) 对每条 `vbranch` 构造候选分支区域并做结构化校验：可达性、后支配、无侧出口、单入口；失败则报错。
4) 生成 PTX 控制流：
   - `setrpc`：用于解析 join；必要时保留一个“逻辑值”以满足程序读 `CSR_RPC` 的语义（原型阶段可按需最小化）。
   - `vbranch`：生成 per-thread predicate + `@p bra` 的分支；组织 then/else 两条路径，并在末尾跳转到同一个 merge label（join）。
   - `join`：在结构化方案中不再生成指令级 `join`，而是落在 merge label 处自然收敛继续执行。

备注：`dump_risk_assessment.md` 已对现有测例 `.dump` 复核，当前范围内 `CSR_RPC` 可静态解析且能定位到 `join`，结构化还原风险可控。

### 2.2 函数调用/返回（选择：PTX `call/ret` + Ventus ABI 语义）

目标：用 PTX 原生 `call/ret` 承载“进入函数/返回 callsite”的控制转移，同时把 Ventus 的寄存器文件/栈/内存视图当作被模拟状态保持语义等价。

**输入约束（不满足则报错退出）**
- `jalr` 仅用于标准返回：`jalr x0, x1, 0`；不支持 jump table / computed goto / 非标准返回。
- 调用深度有限（原型阶段可直接要求无递归或递归极浅）。
- 需要能获得函数边界：ELF 未剥离，包含 `.symtab` 且函数符号为 `STT_FUNC`（或输入来自受控工具链提供等价信息）；否则拒绝或只支持单函数内联（原型阶段建议直接拒绝）。

**翻译规则（与调用相关的最小集合）**
- `jal x1, target`：写入“逻辑返回地址”到被模拟状态 `x1(ra)=pc+4`，然后 `call target`；`ret` 由 PTX 自然回到下一条。
- `jal x0, target`：不写 `ra`，仍 `call target`，返回后立刻 `ret`（等价于“调用后立即返回到本函数 callsite”）。
- `jalr x0, x1, 0`：翻译为 PTX `ret`（注意：控制流不使用 `x1` 的值；`x1` 仅作为程序可见状态保留）。

### 2.3 地址空间映射（选择：方案 B——保留 Ventus 数值地址 + 区间分流）

目标：在 PTX 侧根据 Ventus 32-bit 数值地址把访存路由到 `.shared` 或 `.global`，并能承载 `0x8000_0000~` 的静态数据访问。

**内存模型**
- 在 GPU global memory 中申请一块足够大的连续 buffer 作为“Ventus 全局地址空间”：
  - 2GiB：覆盖 `0x8000_0000~0xFFFF_FFFF`；或
  - 4GiB：覆盖完整 `0x0000_0000~0xFFFF_FFFF`（按你的决定：显存足够即可）。
- 将 Ventus ELF 的静态数据段按其 Ventus 虚拟地址装载到该 buffer 对应偏移（使硬编码地址/PC-relative 计算出来的地址能落到正确数据）。

**访存路由（每次 load/store 执行）**
1) 给定 Ventus 地址 `addr`（32-bit，按 unsigned 处理）。
2) 若 `addr` 落在 shared 区间 `0x7000_0000~0x7FFF_FFFF`：
   - 计算 `offset = addr - CSR_LDS_ventus`（unsigned）。
   - 计算 `ptx_shared_addr = ptx_shared_base + offset`，发 `ld.shared/st.shared`。
3) 否则（global/静态数据等）：
   - 计算 `ptx_global_addr = ventus_global_base + u32(addr)`，发 `ld.global/st.global`。

**关键点**
- 必须同时维护两套“shared 基址语义”：Ventus 数值 `CSR_LDS_ventus` 与 PTX `.shared` 的 `ptx_shared_base`。
- 做加法时不得对 `addr` 做符号扩展（必须视为 unsigned）。

### 2.4 CSR / device ABI（原型期最小映射）

目标：在不追求完整 CSR 行为的前提下，提供跑通测例所需的 ABI 状态。

- `CSR_RPC`：由 `setrpc` 写入，并用于 `vbranch/join` 的结构化解析（必要时也可作为可读状态保留）。
- 线程/线程块标识相关 CSR（如 `CSR_TID/CSR_NUMT/CSR_WID` 等）：优先用 PTX 内建 thread/block 语义替代生成。
- 指针类 CSR（如 `CSR_LDS/CSR_PDS/CSR_KNL` 等）：以 PTX kernel param 传入；其中与地址空间相关者用于 2.3 的映射计算。
- 原型阶段除上述需要的 CSR 外，其它 CSR 视为只读且无副作用。

### 2.5 `barrier`

目标：把 Ventus `barrier` 映射为 CUDA `__syncthreads()` / PTX `bar.sync`。

硬约束（原型阶段写死）：`barrier` 不得出现在发散路径上；若分析/结构化无法证明该点已收敛，直接报错退出。

---

## 3. 建议的 bring-up 顺序（便于尽快跑通测例）

1) 先跑 `vecadd`：验证 `setrpc + vbranch + join + endprg` + 少量 `V` 算术与基本访存。
2) 再验证地址空间路由：shared/global 的读写（`vlw12.v/vsw12.v` 等）。
3) 再验证 ABI/CSR：参数读取、thread id 计算、`CSR_LDS/CSR_KNL` 等端到端一致。
4) 最后引入 `barrier`（确保只在收敛点出现），扩展到 `rodinia/opencl` 其余 6 个测例。

