# Uniform Datapath SASS 小实验（sm_89 / RTX 4090）

这套实验用“手写 PTX + nvcc 编译 + cuobjdump/nvdisasm 反汇编”的方式，观察 **SASS 里的 Uniform Datapath 指令**（典型特征：`UR*` 寄存器、`UP*` uniform predicate、以及 `U*` 前缀的指令如 `UIADD3/UIMAD/UISETP/ULDC/...`）。

环境（本机探测结果）：CUDA 13.1，GPU: RTX 4090（`sm_89`）。

## 运行

在本目录：

```bash
./run_uniform_sass.sh
# 或者指定 ptxas 优化级别（有时 -O0 更“显眼”）
PTXAS_O=-O0 ./run_uniform_sass.sh
```

输出：
- `build/*.sass`：`cuobjdump --dump-sass` 的反汇编
- `build/summary.txt`：每个样例的 `UR` 引用次数 + 发现的 `U*` mnemonic 列表

快速定位关键行：

```bash
grep -nE '\bUR[0-9]+|\bUP[0-9]+|\bUIADD3\b|\bUIMAD\b|\bUISETP\b|\bS2UR\b|\bULDC\b' build/uadd_brauni.sass
```

## 关键发现（可复现）

### 1) “值是 uniform”并不等于一定生成 Uniform Datapath 指令

例如 `ptx/add_uniform.ptx` 中 `a` 是 kernel param（对 warp 来说显然是 uniform），但在 `PTXAS_O=-O3` 时，SASS 仍然用普通 datapath 指令（如 `IADD3`）在 `R*` 寄存器上计算。

在本机 `sm_89 + -O3` 下，**仅靠“表达式数学上 uniform”并不足以触发 `UIADD3/UIMAD/...`**。

### 2) `bra.uni` 是一个非常强的触发条件（让 ptxas 走 uniform 分支/谓词/ALU）

对比：
- `ptx/branch_uniform.ptx`（普通 `bra`）
- `ptx/branch_uniform_brauni.ptx`（`bra.uni`）

`branch_uniform_brauni` 的 SASS 会出现：
- `S2UR`（把 special register 直接读到 `UR`）
- `ULDC`（把常量/参数读到 `UR`）
- `UISETP...`（用 `UP0` 这类 uniform predicate 做比较）

这说明：**当 PTX 显式声明某个控制流是 uniform（`bra.uni`），ptxas 更愿意把相关计算/比较放到 uniform datapath。**

### 3) 常用整数指令例子：`UIADD3` 与 `UIMAD`

- `ptx/uadd_brauni.ptx`
  - 逻辑：`t = ctaid.x + n; if (t >= 123) return;`（全是 uniform，并使用 `bra.uni`）
  - SASS 里可以看到：`S2UR`、`ULDC`、`UIADD3`、`UISETP...`

- `ptx/uimad_brauni.ptx`
  - 逻辑：`t = ctaid.x * 7 + n; if (t >= 1000) return;`（全是 uniform，并使用 `bra.uni`）
  - SASS 里可以看到：`UMOV`、`UIMAD`、`UISETP...`

这两个样例基本回答了“哪些条件下会出现 Uniform Datapath SASS”：

1. 计算链条必须是 **warp-uniform**（只依赖 CTA 级 uniform 的 special registers，如 `SR_CTAID.*`，以及 kernel params/常量等）。
2. 最关键：用 **`bra.uni`**（或同类 `.uni` 控制流标注）把 uniform 性质“显式喂给 ptxas”，它就会把比较/加法/乘加等放到 `UR/UP` 上，并生成 `UIADD3/UIMAD/UISETP` 这类指令。

### 4) 一旦 uniform 值需要“回到 R 域”参与数据路径，uniform ALU 往往就消失

对比：
- `ptx/uadd_brauni.ptx`：`t = ctaid.x + n` 仅用于分支谓词，SASS 能看到 `UIADD3/UISETP`（`UR/UP`）。
- `ptx/uadd_brauni_store.ptx`：`t = ctaid.x + n` 同时用于分支谓词和 `st.global` 写回（`out[tid] = t`），在本机 `sm_89 + -O3` 下，SASS 回退为 `IADD3/ISETP`（`R/P`），没有出现 `UIADD3/UISETP`。

这提示：ptxas 很可能不愿意为“需要进入 vector datapath 的值”做 `UR <-> R` 的跨域搬运，因此你很难靠 PTX 写法稳定获得“把通用标量计算放到 uniform datapath 执行、再把结果广播到各 lane”这种效果。

### 5) `-O0` 会让一些 UR 相关指令更容易“冒出来”

例如 `build/add_uniform_O0.sass` / `build/add_nonuniform_O0.sass` 中能看到 `R2UR`（把 `R` 的值写进 `UR`）。

这更像是 toolchain/ABI/寄存器搬运相关的行为，不一定代表你写的整数运算被 scalarize 了；但它确实属于 Uniform datapath 指令家族。

### 6) `shfl`/位运算构造的“warp-uniform”值，并不会自动触发 uniform ALU

- `ptx/uadd_brauni_shfl_tid0.ptx`：用 `shfl.sync.idx` 广播 lane0 的 `tid.x`，再配合 `bra.uni` 做比较分支；在本机 `sm_89 + -O3` 下仍然是 `IADD3/ISETP`。
- `ptx/uadd_brauni_tidbase.ptx`：`tid_base = tid.x & ~31`（warp 内恒定）再配合 `bra.uni`；在本机 `sm_89 + -O3` 下仍然是 `IADD3/ISETP`。

这意味着：即使你能在 PTX 里“构造出数学上 warp-uniform 的值”，ptxas 也未必会把它识别/搬运进 `UR`，从而用 `UI*` 指令承载；至少在这组样例里看不到这样的自动 scalarize。

## 文件索引

- `ptx/uadd_brauni.ptx`：触发 `UIADD3`
- `ptx/uimad_brauni.ptx`：触发 `UIMAD`/`UMOV`
- `ptx/branch_uniform_brauni.ptx`：触发 `S2UR`/`ULDC`/`UISETP`
- `ptx/branch_uniform.ptx`：对照（无 `.uni`）
- `ptx/uadd_brauni_store.ptx`：对照（uniform 值同时用于 store，uniform ALU 消失）
- `ptx/uadd_brauni_shfl_tid0.ptx`：对照（shfl 广播构造 warp-uniform，但未触发 uniform ALU）
- `ptx/uadd_brauni_tidbase.ptx`：对照（tid 位运算构造 warp-uniform，但未触发 uniform ALU）
- 其他 `add_*/muladd_*/shift_*/and_*`：对照用，展示“仅数学 uniform 不一定触发 uniform datapath”

## 参考资料（便于继续挖）

- PTX ISA：`.uni` 语义（`bra{.uni}`/`call{.uni}`/`ret{.uni}`）见 `https://docs.nvidia.com/cuda/pdf/ptx_isa_8.5.pdf`
- Uniform Datapath 指令列表（`UIADD3/UIMAD/UISETP/ULDC/...`）见 `https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html`
- NVIDIA 论坛：`.uni` 用法讨论 `https://forums.developer.nvidia.com/t/how-to-use-the-uni-suffix/11542`
- NVIDIA 论坛：关于“能否显式控制 uniform datapath”的回答（结论：不能）`https://forums.developer.nvidia.com/t/understanding-uniform-registers/347103`

## 对本项目（Ventus -> ptx 静态二进制翻译工具）的启示

ventus的标量指令功能直接对应SASS uniform，但ptx生成SASS uniform不稳定
相关模拟方案见[这里](../../scalar_uniform_ptx.md)

fast 方案可以作为优化，但不能作为正确性基础；要保证 Ventus 的 warp-uniform 标量语义，仍然必须以 safe(WarpCtx + leader 执行/广播) 兜底。
为什么 fast 不能单独保证正确

PTX 语义是 per-thread，ptxas 是否生成 UR/UP 与 U* 指令不可控且跨版本不稳定；你无法把“会生成 uniform datapath”当作语义前提。
现有实测还显示 uniform datapath 的能力边界很窄：当 uniform 计算结果需要进入数据路径（例如写回 out[tid]）时会回退到 R/P（uadd_brauni_store.ptx (line 1)）；用 shfl/tid&~31 构造的“数学上 warp-uniform”值也没触发 UI*（uadd_brauni_shfl_tid0.ptx (line 1)、uadd_brauni_tidbase.ptx (line 1)）。这基本否掉了“用 uniform datapath 承载通用标量运算并广播”的可依赖性。
fast 仍然“可行”的方式（但要靠 safe 托底）

只在你能静态证明 full-mask 且不早退 的收敛区间：把 WarpCtx 热点值缓存到普通寄存器做冗余计算（就算编成 IADD3 也仍然正确），在进入可能再次发散前回写 WarpCtx；控制流层面尽量用 call.uni 争取更好代码生成即可。
是否值得继续探究

如果目标是“找到稳定 PTX 写法强制产生 uniform datapath 并用于通用标量语义”，目前证据指向投入回报很低。
更值得继续的是：围绕 safe 做“收敛区缓存/flush 策略 + .uni 标注”的性能优化闭环。
