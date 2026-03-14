# PTX Direct Call 参数 ABI 微基准实验

本目录对应 [`doc/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md`](../../doc/TEMP_PTX_DIRECT_CALL_PARAM_ABI_BRAINSTORM.md) 里讨论的下一步实验：先不改主线 `sbt/ptx_emit.cpp`，而是用一组可生成的手写 PTX 微基准，直接观察 `ptxas`/SASS 对不同 direct-call ABI 形式的反应。

## 目标

回答下面几个问题：

- 当前 `vctx` 方案与 `.param value ABI` 相比，PTX 文本体积差多少。
- 当 ABI 很大时，`ptxas` 会不会把大量状态继续落到 local/stack，还是能显著压缩。
- 分散参数与聚合 blob 参数，哪一种更容易被 `ptxas` 优化。
- helper 很短时，`ptxas` 是否会直接消掉 `CALL` 边界。
- “callee 只真正使用少量状态”时，大接口里剩余状态能否被明显收缩。

## 三组对照

- `vctx`
  - 模拟当前实现：caller 先把全部状态 `st.local` 到 `vctx`，callee 再 `ld.local` 全量读回，返回前写回，caller 返回后再恢复。
- `value_params`
  - 把全部状态拆成离散标量接口；输入仍是 `.param .b32`，返回侧由于 PTX 不接受“多 `.param` 返回值”，改用多 `.reg` return。
  - 因此它更像“分散 value ABI 的乐观上界代理”，不是最终 PTX ABI 语法承诺。
- `value_blob`
  - 仍然是 value ABI，但输入/返回值改成单个聚合 `.param .b8 blob[...]`。

## 矩阵设计

默认矩阵：

- `state_words`: `32`, `128`, `256`
- `use_mode`:
  - `hot4`: callee 只真正修改前 4 个状态；caller 仍会校验全部状态，其余状态必须以 passthrough 语义被保留
  - `hot4_dead`: 旧版 deadcode 情节；callee 只真正修改前 4 个状态，caller 也只消费前 4 个状态，其余状态在 caller 侧不可观察
  - `full`: callee 修改全部状态，caller 对全部状态做 checksum
- `helper_ops`: `4`, `32`
  - 用来控制 helper “短小”还是“有一定计算体量”
- `abi`: `vctx`, `value_params`, `value_blob`

默认执行 `./run_experiment.py` 会生成 `3 x 3 x 2 x 3 = 54` 个 case。

其中报告主分析通常聚焦 `hot4/full` 这 `36` 个 case，`hot4_dead` 作为补充对照单独解读。

这组矩阵足够先回答文档中的关键争议：

- `hot4` 用来观测“大 ABI 中大部分状态无实际热点使用”时，后端能否自己压缩。
- `full` 用来观测“full-state value ABI”在最坏情况下是否比 `vctx` 更糟。
- `4` vs `32` 用来观测 helper 边界在短函数与稍长函数下的变化，包括可能的自动内联。

## 运行

在本目录执行：

```bash
./run_experiment.py
```

常用参数：

```bash
# 指定架构
./run_experiment.py --arch sm_89

# 降低 ptxas 优化级别，观察 call 边界是否保留
./run_experiment.py --ptxas-opt-level=-O0

# 只跑一部分 case
./run_experiment.py --match value_params_s256_full_o32

# 自定义矩阵
./run_experiment.py \
  --state-words 64 128 \
  --helper-ops 8 32 \
  --modes hot4 hot4_dead full \
  --abis vctx value_params value_blob

# 运行 deadcode 对照矩阵
./run_experiment.py --modes hot4_dead --out-dir build_hot4_dead
```

依赖：

- `ptxas`
- `cuobjdump`
- `g++`
- CUDA Driver API 运行环境（`libcuda.so` + 可用 NVIDIA GPU）

当前环境已探测到：

- `ptxas`: `/usr/local/cuda/bin/ptxas`
- `cuobjdump`: `/usr/local/cuda/bin/cuobjdump`

## 输出

脚本会把结果写到 `build/`：

- `build/generated_ptx/*.ptx`：每个 case 的 PTX
- `build/cubin/*.cubin`：`ptxas` 编译产物
- `build/sass/*.sass`：`cuobjdump --dump-sass` 反汇编
- `build/call_windows/*.txt`：每个 `CALL` 附近的 SASS 片段，方便看 call 边界是否伴随 `LDL/STL`
- `build/summary.csv`：面向表格处理的结果汇总
- `build/REPORT.md`：便于人工阅读的实验汇总

## 运行时功能一致性验证

本目录额外提供了一个 CUDA host wrapper：

- 源码：`runtime_validate.cc`
- 构建脚本：`build_runtime_validate.sh`

构建：

```bash
./build_runtime_validate.sh
```

运行示例：

```bash
# 默认直接加载 build/generated_ptx/*.ptx，经 CUDA Driver API JIT 到当前 GPU
./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 32 \
  --mode hot4 \
  --helper-ops 4 \
  --threads 128

./build_runtime_validate/runtime_validate \
  --module-dir build/generated_ptx \
  --state-words 256 \
  --mode full \
  --helper-ops 32 \
  --threads 128
```

它会做两层校验：

- 把每个 ABI (`vctx` / `value_params` / `value_blob`) 的输出与 CPU 参考实现逐 lane 比较
- 再做 ABI 之间的逐 lane 交叉比较

补充说明：

- wrapper 默认加载 PTX，而不是直接加载 `build/cubin/*.cubin`
  - 原因是默认实验产物按 `sm_75` 生成，换到别的 GPU（例如 `sm_89`）时直接跑 cubin 可能出现 `CUDA_ERROR_NO_BINARY_FOR_GPU`
  - 直接加载 PTX 时，Driver API 会在当前机器上重新 JIT
- 这个微基准 kernel 当前只使用 `%tid.x`，没有使用 `%ctaid.x`
  - 因而 runtime validator 按“单 block、多 lane”方式运行
  - `--threads` 也因此限制为 `<= 1024`

## 如何读结果

优先看 `build/REPORT.md` 的这些列：

- `ptx_bytes` / `ptx_lines`
  - 看 ABI 写法本身造成的 PTX 文本膨胀。
- `registers`
  - 看 `ptxas` 是否因大 ABI 拉高寄存器压力。
- `stack_frame` / `spill_stores` / `spill_loads`
  - `stack_frame` 是 entry kernel 的栈帧；`spill_*` 是 caller + callee 总 spill 字节数。
  - 用它们看 value ABI 是否只是把问题转成栈和 spill。
- `call_count`
  - 如果变成 `0`，通常代表 helper 被内联或 call 边界被消掉。
- `ldl_count` / `stl_count`
  - 看整份 SASS 的 local traffic 是否仍显著存在。
- `call_window_ldl_stl`
  - 专门看 `CALL` 附近局部窗口内的 `LDL/STL`，用于判断 call 边界附近是否还有明显 local-memory 型搬运。

补充说明：

- `value_params` case 在 `ptxas` 下会出现 “`multiple return values found, disabling ABI`” 警告。
- 这正说明 PTX 对“超大分散返回接口”本身就有语法/ABI 边界，因此这组数据应按“上界 proxy”解读。
- 因而正式主对照应优先看 `vctx` 与 `value_blob`，`value_params` 更适合作为探索性旁证。
- `hot4` 的当前实现要求 caller 仍然观察全部状态；因此冷状态不是 dead value，而是“必须正确 passthrough 的冷状态”。
- `hot4_dead` 保留了最初的 deadcode 情节，专门用于对照“冷状态在 caller 侧完全不可观察”时，编译器会删掉多少 ABI 成本。
- `value_params` 一旦触发 `disabling ABI`，其结果应理解为 `ptxas` 的 fallback lowering，而不是标准 PTX direct-call 参数 ABI。
- `spill_*` 与 `LDL/STL` 不是同一量纲：前者是 spill 字节数，后者是 SASS local 指令条数。
- `call_window_ldl_stl` 也不是全部 ABI 边界成本；它只覆盖窗口内可见的 local-memory 指令，不覆盖其它 marshalling 形式。

建议的阅读顺序：

1. 同一组 `state_words/use_mode/helper_ops` 下横向比较 `vctx` / `value_params` / `value_blob`。
2. 再比较 `hot4` 与 `full`，看“无用状态”是否真的被后端压缩。
3. 最后比较 `helper_ops=4` 与 `32`，看短 helper 是否更容易直接失去 `CALL`。

## 解释边界

这套实验故意只回答“PTX direct-call ABI 形状”对 `ptxas` 的影响，不掺入 Ventus 指令翻译、地址空间映射、SIMT stack、CSR 等主线 lowering 复杂度。

因此它适合回答：

- `.param` value ABI 有没有继续追的价值
- 分散参数还是聚合 blob 更值得优先验证
- 是否值得把主线实现重心从 `vctx` 转向 value ABI

但它不能单独回答：

- 与真实 Ventus kernel 端到端功能是否完全一致
- 三种 ABI 变体在运行时是否与当前 `vctx` 方案严格语义等价
- 主线 `sbt/ptx_emit.cpp` 改造后的真实收益是否与微基准完全一致

其中第二点现在已经可以用本目录的 `runtime_validate.cc` 对单个 case 做抽查验证；但它还没有被并入 `run_experiment.py` 去自动跑完整矩阵。

后者应在本实验给出方向性结论后，再进入第二阶段，用真实 `sbt_ptx` 输出做实测。

已完成的一轮实测报告见：

- `EXPERIMENT_REPORT.md`
