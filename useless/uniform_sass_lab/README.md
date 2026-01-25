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

### 4) `-O0` 会让一些 UR 相关指令更容易“冒出来”

例如 `build/add_uniform_O0.sass` / `build/add_nonuniform_O0.sass` 中能看到 `R2UR`（把 `R` 的值写进 `UR`）。

这更像是 toolchain/ABI/寄存器搬运相关的行为，不一定代表你写的整数运算被 scalarize 了；但它确实属于 Uniform datapath 指令家族。

## 文件索引

- `ptx/uadd_brauni.ptx`：触发 `UIADD3`
- `ptx/uimad_brauni.ptx`：触发 `UIMAD`/`UMOV`
- `ptx/branch_uniform_brauni.ptx`：触发 `S2UR`/`ULDC`/`UISETP`
- `ptx/branch_uniform.ptx`：对照（无 `.uni`）
- 其他 `add_*/muladd_*/shift_*/and_*`：对照用，展示“仅数学 uniform 不一定触发 uniform datapath”
