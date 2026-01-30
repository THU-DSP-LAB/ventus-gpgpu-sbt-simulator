# PTX 上软件化 Ventus SIMT stack：控制流方案

本文目标：回答“PTX 是否支持软件显式控制当前 mask”，并给出一套在 PTX 上**软件实现 Ventus SIMT stack 控制流**（`setrpc/vbranch/join`）的可执行方案（仅设计，不写实现代码）。

## 1. PTX 是否支持“软件显式控制当前 mask”？

结论分两层：

- **不能**：PTX 没有类似“写一个寄存器就能直接改硬件执行掩码（hardware active mask）”的通用接口。线程是否处于硬件 active，主要由硬件分歧/收敛机制与控制流决定，软件不可直接指定“接下来只让这些 lane 执行”。
- **能（等价替代）**：PTX 支持 predicate（谓词寄存器）与 predicated instruction，可在软件里维护一个 **virtual mask（软件 mask）**，并把所有会产生副作用的指令（寄存器写回、load/store、原子、打印等）都在 `@p_act` 下执行，从而实现“只让某些 lane 生效”的效果。
  - 同时，PTX 提供 warp 级投票/ballot 一类能力，允许把“每 lane 的条件”汇总成一个 32-bit 掩码，便于在软件里更新 `cur_mask`。

因此：PTX **不支持直接写硬件 mask**，但完全可以通过“谓词化 + ballot + warp-uniform 控制流”实现 Ventus SIMT stack 所需要的 mask 管理语义。

## 2. 设计目标与核心思路

### 2.1 目标

- 在 PTX 内实现与 Ventus 一致的 warp 级状态机：
  - warp 只有一个“Ventus PC”（统一控制流），每条指令在 warp 内同步推进；
  - `cur_mask` 决定哪些 lane 对指令产生副作用；
  - `setrpc/vbranch/join` 通过软件 SIMT stack 显式驱动分歧/收敛。

### 2.2 核心思路

- **warp-uniform 控制流**：整个 kernel 尽量避免使用“每线程不同 predicate 的 `bra`”造成硬件分歧；所有 `bra` 都基于 warp-uniform 条件（例如 `pc` 的值、`sp>0`、popcount 比较结果等）。
- **软件 mask 驱动副作用**：每条“可能产生副作用”的指令都写成 `@p_act` 形式，`p_act` 由 `cur_mask` 与 `laneid` 计算。
- **软件 SIMT stack**：维护一个最多 32 深度的栈，条目包含 `{rpc, newpc, newmask}`，并严格按 Ventus 规则 push/pop。

## 3. 状态表示（warp 级）

以下状态对一个 warp 内的所有线程应当保持一致（warp-uniform）：

- `u32 pc`：当前 Ventus PC。
- `u32 rpc`：对应 `CSR_RPC` 的值（Ventus reconvergence PC）。
- `u32 cur_mask`：当前 software mask，32-bit，每 bit 对应一个 lane。
- `i32 sp`：SIMT stack 栈顶指针（0 表示空栈）。
- `stack[32]`：每项包含：
  - `u32 st_rpc[i]`
  - `u32 st_newpc[i]`
  - `u32 st_newmask[i]`

每个线程还需要：

- `u32 laneid`：线程在 warp 内的 lane id（0..31）。
- `u32 lane_bit = 1u << laneid`。
- `pred p_act = ((cur_mask & lane_bit) != 0)`。

备注：为了让 `pc/rpc/newpc` 可在 PTX 中高效比较/跳转，建议在 SBT 时把“原始 Ventus PC”映射为连续的 **block_id**（或 instruction_id）。下文的 `pc/rpc/newpc` 可理解为该 id。

## 4. 执行框架（dispatch 循环）

为了保持 warp-uniform 控制流，需要一个“按 `pc` 分发”的统一循环。形式上类似：

- `while (true) { switch(pc) { case ...: 执行该基本块/该条指令对应的 PTX 序列; pc = next; continue; } }`

在 PTX 里不一定要真的用 `switch` 语法，关键是：

- 所有跳转都由 warp-uniform 条件控制；
- `pc` 的更新是 warp-uniform 的；
- 基本块内部对数据/访存的副作用由 `@p_act` 控制。

这样，Ventus 的“warp 级单 PC + mask”模型在 PTX 中就能保持为“所有线程同一路径执行，但只有 active lane 生效”。

## 5. 三个关键指令的翻译语义（软件化）

### 5.1 `setrpc rd, rs1, offset`

软件语义：更新 `rpc`（即 `CSR_RPC`），并返回计算值。

- `rpc = rs1 + sext(offset)`（warp-uniform）
- `rd = rpc`（若 Ventus 语义要求写回；若 rd==x0 则忽略）

要点：`rpc` 必须是 warp-uniform 的；因此 `rs1` 与立即数也应当来自 warp-uniform 计算路径（典型情况：编译器把 join 位置编码为常量/PC-relative 计算）。

### 5.2 `vbranch`（以 `vbeq/vbne/...` 为代表）

目标：在不引入硬件分歧的情况下，计算两条路径的 mask、选择先执行 PATH1、并按规则 push 两个栈条目。

步骤（概念流程）：

1) 计算每 lane 的分支条件（仅对 active lane 有意义）
- `pred p_cond = (vs2 OP vs1)`（每线程各自计算）
- `pred p_take = p_act && p_cond`

2) 汇总得到路径 mask（warp-uniform）
- `u32 m_take  = ballot(p_take)`
- `u32 m_not   = cur_mask & ~m_take`

3) 处理“某路径 mask 全 0”的退化情况
- 若 `m_take == 0`：只执行 not-taken 路径（不压栈），`pc = pc+4`（或相应 PATH）且 `cur_mask` 不变。
- 若 `m_not == 0`：只执行 taken 路径（不压栈），`pc = pc+offset` 且 `cur_mask` 不变。

4) 选择 PATH1（线程数更少的分支先执行）
- `cnt_take = popcount(m_take)`
- `cnt_not  = popcount(m_not)`
- 若 `cnt_take <= cnt_not`：
  - `path1_mask = m_take`, `path1_pc = pc+offset`
  - `path2_mask = m_not`,  `path2_pc = pc+4`
- 否则相反。

5) 压栈两次（严格按 Ventus 规则的等价数据）
- push1：`{ rpc, newpc=rpc,      newmask=cur_mask }`
- push2：`{ rpc, newpc=path2_pc, newmask=path2_mask }`

6) 进入 PATH1
- `cur_mask = path1_mask`
- `pc = path1_pc`

关键点：

- `ballot/popcount` 的输入可以是每线程 predicate，但输出是 warp-uniform 的 32-bit 值，因此后续的路径选择与 `pc/cur_mask` 更新可保持 warp-uniform，不触发硬件分歧。

### 5.3 `join`

目标：实现“当且仅当 `pc == top.rpc` 时弹栈并更新 `{pc, cur_mask}`；否则 no-op”的语义，并支持嵌套分支共享同一 join（可能连续弹栈）。

流程：

- `if (sp == 0) { /* no-op */ }`
- `else if (pc != st_rpc[sp-1]) { /* no-op */ }`
- `else { pc = st_newpc[sp-1]; cur_mask = st_newmask[sp-1]; sp--; }`

并且，为支持“共享同一 join 造成连续弹栈”的情况，建议在 join 处写成：

- `while (sp>0 && pc==st_rpc[sp-1]) { pc=...; cur_mask=...; sp--; }`

该 `while` 的条件完全 warp-uniform，因此不会引入硬件分歧。

## 6. 常规指令在 software mask 下的执行规则

原则：只要某条指令对“inactive lane”不应产生副作用，就必须写成 `@p_act` 形式。

- **向量寄存器（per-thread）**：自然映射到 PTX 的线程寄存器；对这些寄存器的写回用 `@p_act` 保护即可。
- **访存（`vl*12.v`/`vs*12.v` 等）**：load/store 必须 `@p_act` 保护，确保 inactive lane 不会发出访存（避免越界/非法地址触发）。
- **产生可见副作用的操作**（原子、打印、与 host 交互等）：同样要求 `@p_act`。

补充：如果某些“标量/warp-uniform”状态需要只更新一次（而不是每 lane 更新一次），可用 `laneid==0` 作为执行者，再用 shuffle/broadcast（或 shared memory）把值同步到其它 lane；这属于标量路径设计，和 SIMT stack 控制流方案相互独立。

## 7. `barrier` 的处理（原型阶段假设）

- 当前阶段把 `barrier` 等同于 CUDA `__syncthreads()`，约束：**不能在发散路径上执行**。
- 在本方案中，控制流本身保持 warp-uniform，但 `cur_mask` 可能不是全 1。

因此建议把 `barrier` 的可用性约束写得更硬：

- 只有当 `cur_mask == FULL_MASK`（例如 0xffffffff 对应 32 lane）时才允许到达 `barrier`；否则语义未定义/直接视为非法输入。

原因：即使你尝试用谓词去“屏蔽” barrier，硬件层面的同步通常无法依赖 predication 安全跳过（容易造成部分线程不参与同步而死锁）。

## 8. 工程化注意事项（会影响方案可落地性）

- **栈溢出**：深度上限 32，若嵌套分支超过上限，必须定义行为（当前阶段可视为非法输入）。
- **`cur_mask==0`**：理论上不应发生；若发生，可以选择“持续 join 弹栈直到 mask 非 0 或栈空退出”，或直接视为非法。
- **性能**：该方案牺牲了硬件分歧带来的并行控制流优势（所有线程都执行同一路径，只是副作用被屏蔽），适合功能仿真/原理验证；一旦追求性能，应考虑 CFG 结构化还原、让硬件做分歧/收敛。

---

这套方案的关键价值是：只依赖 PTX 的谓词化与 warp 投票能力，用 warp-uniform 的 `pc/stack/mask` 状态机复刻 Ventus 的 SIMT stack 语义，从而在不要求 PTX 提供“可写硬件 mask”的前提下实现等价控制流。\
