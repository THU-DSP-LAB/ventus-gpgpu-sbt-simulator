@README.md

本仓库作为 `ventus-env` 的子项目集成使用时，`ventus-env` 位于本仓库的上级目录（`../`），。
* ventus 有关的软件工具使用 `../install/bin` 目录下的可执行程序
* ventus 编译器：`../install/bin/clang -cl-std=CL2.0 -target riscv32 -mcpu=ventus-gpgpu kernel.cl -o kernel.riscv -nodefaultlibs -Wl,${VENTUS_ENV_PATH}/install/lib/crt0.o -Wl,${VENTUS_ENV_PATH}/install/lib/riscv32clc.o -Wl,--gc-sections -L${VENTUS_ENV_PATH}/install/lib -lworkitem -I${VENTUS_ENV_PATH}/installinclude/clc -O1 -Wl,-T,${VENTUS_ENV_PATH}/install/lib/ldscripts/ventus/elf32lriscv.ld -Wl,--init=${KERNEL_FUNC_NAME}  -w  -D__opencl_c_generic_address_space=1 -D__opencl_c_named_address_space_builtins=1  -D__OPENCL_VERSION__=200` 注意替换 ${VENTUS_ENV_PATH} 和 ${KERNEL_FUNC_NAME}
* ventus 反汇编器：`../install/bin/llvm-objdump -d --mattr=+v,+zfinx kernel.riscv > kernel.dump`
* `ventus-env` 具有一系列子仓库列举如下（你可以通过判断它们是否存在来判断上级目录是否为 `ventus-env`）：
  * 编译器 `../llvm`
  * 功能仿真器 `../spike` （对本项目有参考价值）
  * 周期仿真器 `../cyclesim` （对本项目有参考价值）
  * Chisel RTL 及其仿真器 `../gpgpu`
  * OpenCL 实现 `../pocl` （基于 PoCL 开源项目改造，Ventus相关主要在 `lib/CL/lib/CL/devices/ventus`）
  * Ventus 驱动程序 `../driver`
本项目通常不应直接修改上级 `ventus-env` 的源码；仅在做“集成到工具链”的工作（例如 `driver/driver/ptx_device`）时例外。

使用 Ventus ELF 作为输入，目前阶段采用（不要使用 .vmem 文件）：
* `../rodinia/opencl/*/*.riscv`
* `../pocl/build/examples/*/*.riscv`

## tools/ 约束

如果需要新增额外工具脚本/程序/库，必须放在 `tools/` 路径下。
`tools/` 路径下的所有文件满足“单文件可维护性”要求：
* 脚本文件头必须自带四段说明：`背景`、`需求/作用`、`用法`、`实现原理/处理步骤`
* 关键处理步骤需添加简要注释（避免无上下文的黑盒逻辑）
* 设计上应尽量做到“仅阅读该单文件即可理解并维护”

## 文档同步约束

做任何代码/脚本/配置变更后，必须同步检查并更新相关文档（至少包括 `README.md`、`doc/` 下对应文档、必要时 `tools/README.md`）。

使用 OpenSpec 开展工作时，必须将“检查/修改文档”显式纳入执行计划，并在收尾时完成一致性校对。
