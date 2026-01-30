# Ventus 标量（warp-uniform）语义在 PTX 上的落地方案（设计）

本文只做方案设计，不涉及实现代码；目标是补齐“Ventus 标量/warp-uniform 语义在 PTX 上如何保持等价”的可行性论证，并给出一条**正确性优先**、可逐步优化到更高性能/更像 SASS uniform 的路线。

## 0. 背景：为什么这是硬问题

Ventus 的标量指令（RV32* + `zicsr` 等）在 ISA 语义上是 **warp-uniform**：
- 一个 warp 只有一份标量寄存器文件（`x[]`），标量指令的副作用不受 SIMT `cur_mask` 屏蔽。
- 控制流由 warp 级单 PC + SIMT stack 驱动；即使处于发散路径（`cur_mask != FULL_MASK`），warp 仍会继续执行标量指令并更新 `x[]`/CSR/栈等状态。

但 PTX 的执行模型是 per-thread：
- 硬件分歧/收敛依赖 `@p bra` 产生“线程子集执行某条路径”，inactive lane 不会执行路径内任何指令。
- PTX 不显式暴露 SASS 的 uniform datapath（除 `.uni` 修饰少量控制流类指令外），也不保证 ptxas 一定把某段代码编译成 uniform 指令。

因此，在“用硬件分歧/收敛承载 `vbranch/join`”的翻译策略下，Ventus 标量语义会天然遇到一个矛盾：
- Ventus：只要 warp 还有活跃 lane，就应该执行标量指令，并更新 **全 warp 共享** 的标量状态。
- PTX：在发散路径里，只有 active lane 执行指令；如果标量状态被实现成 per-thread 寄存器，就会出现“标量状态分裂”。

> 典型症状：PATH1 中执行了 `lw t0, ...` 并用 `t0` 参与后续计算；PATH2 中某些 lane 在 PATH1 是 inactive，它们切换到 PATH2 后若仍沿用各自的旧 `%t0`，就会与 Ventus 的“warp 共享 `t0` 已更新”不一致。

---

## 1. 目标与我们愿意接受的现实

### 1.1 目标（正确性优先）

在不要求 ptxas 一定生成 SASS uniform 指令的前提下，保证：
- 标量寄存器 `x[]`、与标量相关的 CSR（至少包含 `CSR_RPC`）在语义上是“每 warp 一份”，不会因为发散路径执行而在不同 lane 之间分裂。
- 标量 load/store（例如 xgpr spill/栈访问）在发散路径下仍等价于 Ventus：只要该路径的 active mask 非 0，warp 就执行一次标量访存。
- PATH1/PATH2 切换时，新激活的 lane 能看到最新的标量状态。

### 1.2 关于“同地址同数据 store 不会丢”的澄清

你提到：如果标量 store 的地址与数据在 warp 内完全相同，那么只有部分 lane 执行 store 也不会丢副作用（最终会覆盖/合并成同一结果）。

这点对“**内存最终值**”常成立，但不足以推出“标量语义整体正确”，原因是：
- **标量寄存器状态**：即使 store 等价，标量寄存器的更新也可能被 inactive lane 漏执行，从而在 PATH2 变 active 时读到旧值。
- **标量 load 的结果传播**：只让部分 lane 执行 `lw`，其它 lane 的对应寄存器不会更新；而 Ventus 语义是“warp 共享寄存器已更新”。后续只要这些 lane 重新变 active 并使用该寄存器，就会出错。

因此需要一个显式的“warp 共享标量状态载体”，而不能把标量状态寄托在 per-thread 寄存器隐式一致性上。

---

## 2. 方案总览：分层落地（Safe → Fast）

我们把方案拆成两层，便于先跑通、再追性能：

1) **Safe（默认）**：把 Ventus 标量寄存器/CSR 显式实现成“每 warp 一份”的 `WarpCtx`（存放在 `.shared` 或 global），并用“warp leader 执行 + 广播/重载”的方式在 PTX 上复刻 warp-uniform 语义。  
这层不依赖 SASS uniform datapath，可证正确。

2) **Fast（可选优化）**：在可证明 warp 全活跃（converged/full mask）的区域，把部分标量值缓存到寄存器、使用 `.uni` 控制流、尽量让 ptxas 有机会把计算映射到 SASS uniform。  
这层是“尽力而为”的优化，正确性仍由 Safe 层兜底。

后文重点写 Safe 层，因为它决定可行性。

---

## 3. Safe 层核心：显式 `WarpCtx` + Leader 执行

### 3.1 `WarpCtx`：每 warp 一份的标量状态载体

为避免“标量状态分裂”，把所有 Ventus 标量状态放在一个显式结构体里，并保证同一 warp 的所有线程访问的是同一份：

```c
struct WarpCtx {
  u32 x[256];       // Ventus 标量寄存器文件（至少覆盖实际用到的子集）
  u32 csr_rpc;      // CSR_RPC（若 setrpc/join 的语义需要保留）
  // 可按需加入：sp、其它 CSR 的缓存、调用栈元数据等
};
```

**放置位置建议**：
- 优先 `.shared`：延迟/带宽更友好，且本质是 per-CTA 的“scratchpad”，适合每 warp 独立状态。
- 若 `.shared` 压力过大或 block 规模不可控，可退到 global（正确但慢）。

**索引方式**：
- 计算 `warp_in_block = (threadIdx.x >> 5)`（假设 warp_size=32）
- `WarpCtx* wctx = base + warp_in_block * sizeof(WarpCtx)`

### 3.2 Leader lane：在“当前 active 子集”里选一个执行者

发散路径里 lane0 可能 inactive，不能硬编码 “lane0 执行标量”。

做法：每次需要执行标量副作用时，在“当前 active mask”中选出一个 leader lane：

- `mask = activemask.b32`（当前硬件 active lane mask）
- `lowest = mask & (-mask)`（取最低位 1）
- `leader_lane = bfind(lowest)`（由于 `lowest` 只有单 bit，msb/lsb 等价）
- `p_leader = (laneid == leader_lane)`

然后所有标量指令都改写成：
- **只由 leader** 对 `WarpCtx` 做读改写、对内存做真实副作用
- **所有当前 active lane** 通过 `shfl.sync` 或从 `WarpCtx` 读取获得更新后的标量值（以便后续向量指令使用）
- 当前 inactive lane 不需要立刻同步；它们将来变 active 时会通过读取 `WarpCtx` 得到最新状态

> `activemask.b32`、`bfind`、`shfl.sync` 均是 PTX 层可用的通用工具（具体 ISA 版本需实现阶段确认）。

---

## 4. 指令级改写规则（Safe 层）

为便于描述，下文把“Ventus 标量寄存器 xN”对应的 `WarpCtx.x[N]` 记作 `X[N]`。

### 4.1 标量算术/逻辑：`add rd, rs1, rs2`（示意）

语义：warp 执行一次，更新 `X[rd]`。

改写：
1) leader 从 `WarpCtx` 读取操作数
2) leader 计算结果并写回 `WarpCtx`
3) 所有 active lane 通过 `shfl.sync` 获得结果（可选，但通常需要）

伪 PTX：

```ptx
// 计算 leader
activemask.b32   %m;
not.b32          %t, %m;
add.u32          %t, %t, 1;        // %t = -%m（two's complement）
and.b32          %low, %m, %t;     // lowest = m & -m
bfind.u32        %leader, %low;
mov.u32          %lane, %laneid;
setp.eq.u32      %p_leader, %lane, %leader;

// leader 执行标量 op：X[rd] = X[rs1] + X[rs2]
@%p_leader ld.shared.u32  %a, [wctx + off(X[rs1])];
@%p_leader ld.shared.u32  %b, [wctx + off(X[rs2])];
@%p_leader add.u32        %r, %a, %b;
@%p_leader st.shared.u32  [wctx + off(X[rd])], %r;

// 广播到当前 active lane，保证后续 vector op 读到一致的 %r
shfl.sync.idx.b32 %r_all, %r, %leader, %m;
```

说明：
- 如果后续不需要把 `X[rd]` 当作向量指令的标量源（或马上会再次从 `WarpCtx` 读取），可以省略 `shfl`，改为按需 `ld.shared`。
- 对 64-bit 值可做两次 `shfl`（lo/hi）。

### 4.2 标量 load：`lw rd, imm(sp)`（示意）

语义：warp 执行一次，从内存读取并更新 `X[rd]`。

改写：leader 计算地址并执行 load，然后写回 `WarpCtx` 并广播。

关键点：
- 标量地址通常来自 `sp`/`CSR_LDS` 这类 warp-uniform 值，所以由 leader 执行一次 load 在语义上成立。
- 如果标量 load 的地址指向“会被向量线程写入的数据”，那本身就存在竞态/内存模型问题，应作为输入约束明确（原型阶段建议不支持）。

### 4.3 标量 store：`sw rs, imm(sp)`（示意）

语义：warp 执行一次，向内存写入。

改写：只由 leader 执行一次 store。

```ptx
@%p_leader st.shared.u32 [addr], %val;
```

说明：这里不要求其它 active lane 也执行 store；Ventus 语义也是一次写入。

### 4.4 “PATH 切换”时的标量一致性：为什么这能工作

Safe 层的关键不在于“当前 active lane 内一致”，而在于“下一次某 lane 变 active 时能看到最新标量状态”。

这一点由两条约束保证：
1) 任何会改变 Ventus 标量状态的操作，都必须落在 `WarpCtx`（共享载体）里；不能只更新某些 lane 的私有寄存器而不落盘。
2) 任何向量指令要使用的标量源，都从 `WarpCtx` 获取（直接 `ld`，或通过 leader 广播拿到）。

因此：
- PATH1 执行期间，哪怕只有一部分 lane active，leader 仍会把标量更新写入 `WarpCtx`。
- 当硬件切换到 PATH2 时，PATH2 的 active lane 在使用标量值前会从 `WarpCtx` 读到最新值，于是不会出现“PATH2 lane 读到 PATH1 之前的旧标量状态”的问题。

### 4.5 CSR：把“warp-uniform CSR”也当作标量状态处理

在 Ventus 语义中，除 `CSR_RPC` 外其它 CSR 原型阶段可视为只读，但它们常用于：
- 计算 thread/warp/workgroup 的标识（`CSR_TID/NUMT/WID/...`）
- 获取 metadata buffer 基址（`CSR_KNL`）
- 获取 shared/private 基址（`CSR_LDS/CSR_PDS`）

建议把 CSR 读取也放入 `WarpCtx` 或作为“leader 计算一次并广播”的值来使用，避免在发散路径里出现 per-thread 不一致。

特别注意：
- CUDA/ PTX 内建的 `%tid.x` 是 per-thread；若 Ventus 的 `CSR_TID` 定义为“warp 内最小 tid”，则需要显式转成 warp-uniform（例如 `tid_base = (tid.x & ~31)`）。

### 4.6 与 `.uni` / SASS uniform 的关系（Safe 层的态度）

Safe 层不依赖 `.uni` 或 SASS uniform。

但 Safe 层的结构本身是“一个 lane 执行 + 其它 lane 只做搬运/广播”，这在工程上给了后续优化空间：
- 若某些平台/ptxas 版本能把 leader-only 代码段识别为 uniform 并用 `UR` 指令承载，属于加分项；
- 如果识别失败，仍然正确（只是慢）。

---

## 5. Fast 层优化（可选）：尽量让 ptxas 生成 uniform

这部分是“想象中理想状态”，不作为正确性前提，但可以写在路线图里。

### 5.1 只在“可证明 converged”区域用寄存器缓存

当确定当前 warp 是 full mask（例如在 `join` 后的后支配点，且不存在提前退出导致的长期 inactive lane），可以：
- 把 `WarpCtx.x[]` 的热点子集 load 到寄存器缓存（每 thread 一份，但值相同）
- 在该 converged 区域内直接用寄存器做标量计算
- 在进入下一次可能发散的区域前，把缓存回写到 `WarpCtx`

这样可以显著减少 `.shared` 访存与 `shfl`。

### 5.2 控制流上尽量用 `.uni`

对 **已静态证明 warp-uniform** 的控制流（例如由 `WarpCtx` 的标量值决定的分支/循环），尽量生成：
- `bra.uni`
- `call.uni`

目的不是“强制 uniform datapath”，而是减少不必要的 divergence stack 行为，让 ptxas 更有空间做 uniform 推断。

### 5.3 验证手段（建议写进工程验证闭环）

要判断“是否真的映射到 SASS uniform”，只能做事实验证：
- 编译到 cubin 后用 `nvdisasm`/`cuobjdump --dump-sass` 检查是否出现 `U*` 指令/`UR` 寄存器
- 在不同 ptxas 版本/不同 `-O` 下对比

并且要接受现实：这类映射不具备跨版本稳定性，必须有 Safe 层兜底。

### 5.4 实验补充：`.uni` 的收益边界（sm_89 + CUDA 13.1 实测）

仓库里有一组可复现实验：`lab/03_sbt_feasibility/try/sass_uniform/README.md`（手写 PTX → `nvcc` → `cuobjdump` 观察 `UR/UP` 与 `U*` 指令）。

在这组实测里，除了“`bra.uni` 能触发 uniform datapath”之外，还有几个对 Fast 层设计很关键的边界：

- **uniform ALU 更像“控制流/谓词优化”，而非通用 scalarize**：当 uniform 计算只用于分支谓词（不需要把结果当作数据写回/参与向量数据路径）时，更容易看到 `UIADD3/UISETP/UIMAD/...`（例如 `uadd_brauni` / `uimad_brauni`）。
- **同一个 uniform 值一旦需要进入数据路径，uniform ALU 往往就消失**：把 `t = ctaid.x + n` 同时用于 `bra.uni` 和 `st.global`（`out[tid] = t`）后，在 `sm_89 + -O3` 下回退为 `IADD3/ISETP`（对照样例 `uadd_brauni_store`）。
- **“数学上 warp-uniform”不等于 ptxas 会搬进 `UR`**：通过 `shfl.sync.idx` 广播或 `tid.x & ~31` 位运算构造的 warp-uniform 值，在 `sm_89 + -O3` 下仍未观察到 ptxas 自动改用 `UI*` 指令（对照样例 `uadd_brauni_shfl_tid0` / `uadd_brauni_tidbase`）。

因此，Fast 层更现实的定位是：**用 `.uni` 降低分歧控制流开销、并尽量让“分支判定链条”落到 uniform datapath**；而“把任意标量算术都稳定映射到 uniform datapath 并自动广播结果”在 PTX 层很难当成可依赖的能力（必须继续由 Safe 层兜底）。

---

## 6. 输入约束（建议显式写进可行性分析）

为了让上述方案可证正确、实现成本可控，建议把以下约束写硬：

- warp_size 固定为 32（与 Ventus SIMT stack 深度一致）。
- 标量指令只依赖 warp-uniform 数据（`x[]`/CSR/立即数），不会读取 per-thread 值（原型阶段可直接拒绝这类形态）。
- 标量 load/store 的地址是 warp-uniform，并且不会与向量线程产生未同步的数据竞争（否则原始 Ventus 程序语义本身就不清晰）。
- `barrier` 不出现在发散路径（已有约束）；同时避免在部分 active lane 下执行任何需要全体线程参与的同步原语。

---

## 7. 小结：这条路为什么“能落地”

核心要点只有一句话：

> 不要指望 PTX 寄存器天然就是“每 warp 一份”；把 Ventus 标量状态显式落到 `WarpCtx`，并在发散路径里用“从 active lane 里选 leader 执行一次”的方式维持 warp-uniform 语义。

在这个前提下：
- 无论 ptxas 是否生成 SASS uniform，都能保证正确性；
- 一旦未来确认某些写法能稳定映射到 uniform datapath，再逐步把热点从 Safe 层提升到 Fast 层即可。
