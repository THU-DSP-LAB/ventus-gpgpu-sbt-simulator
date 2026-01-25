# 00_prototype：Ventus（simple.S） -> PTX -> NVIDIA GPU

## 目标
提供一个最小可运行的端到端原型，通过在 NVIDIA GPU 上执行等效的 PTX 内核，验证 `simple.S` 中使用的 Ventus 向量指令子集。

该原型专门检验核心映射思路：
- Ventus 的“向量”lane 对应 NVIDIA 的 warp lane。
- 以每 lane 的方式（SIMT 风格）访问内存：通过基指针加上由 lane id 派生的索引来计算地址。

## 输入 / 产物
- `ventus/simple.S`：Ventus 汇编源文件。
- `ventus/compile.sh`：Ventus 工具链编译测试用例的方法（保留作参考）。
- `ventus/simple.dump`：已编译 RISCV/Ventus 二进制的反汇编输出。
- `ventus/simple.vmem`：测试用例使用的原始指令字（十六进制）。
- `ptx/simple.ptx`：手工编写的等效 PTX 内核。

## 已确认的指令语义（子集）
根据测试用例和澄清：
- `vid.v`：lane id，范围 [0, 31]（映射到 PTX 的 `%laneid`）。
- `vsll.vi`：立即数控制的整数左移。
- `vadd.vx` / `vadd.vv`：逐 lane 的整数加法。
- `vlw12.v` / `vsw12.v`：使用 imm12 寻址形式的 32 位加载/存储；寄存器为 32 位（无扩展宽度问题）。
- `endprg`：结束 warp（无其他副作用）。

## 寻址模型
原始 Ventus 程序使用固定的地址基址 `0x90000000`。

在 NVIDIA GPU 上我们不强制将内存分配到该地址。相反：
- PTX 内核接收一个 `base` 指针参数（设备指针）。
- 我们将该指针视为对应 Ventus 中 `0x90000000` 的有效基址。

在此测试用例中，有效访问可简化为：
- `addr = base + (laneid << 2)`

## PTX 内核行为
`ptx/simple.ptx` 实现：
- `lane = %laneid`
- `x = *(u32*)(base + 4*lane)`
- `x = x + lane`
- `*(u32*)(base + 4*lane) = x`

## 如何运行
在仓库根目录运行：

```bash
./lab/00_prototype/smoke_simple.sh
```

该脚本将执行以下步骤：
1) 将 `ptx/simple.ptx` 编译为 `lab/00_prototype/build/simple.cubin`
2) 构建主机运行程序到 `lab/00_prototype/build/run_simple_ptx`
3) 以 `<<<1 block, 32 threads>>>` 启动内核
4) 验证 `out[i] == in[i] + i`，其中 i=0..31

工具链版本会记录在 `lab/00_prototype/build/toolchain.txt` 中。

## 说明
- 内核名为 `vecadd_simple`。
- 该原型使用 CUDA Driver API（`cuModuleLoadData`, `cuLaunchKernel`），因此我们可以直接加载由 PTX 生成的 cubin。
