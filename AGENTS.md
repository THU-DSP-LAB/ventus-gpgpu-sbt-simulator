
@README.md

**临时**：已将整个 ventus 项目（工具链、仿真器、测例等）放在 [ventus-env](./ventus-env) 供当前阶段参考
* ventus 有关的软件工具需要使用 ./ventus-env/install/bin 目录下的可执行程序
* 反汇编：`./ventus-env/install/bin/llvm-objdump -d --mattr=+v,+zfinx kernel.riscv > kernel.dump`
* 子仓库功能：
  * 编译器 `./ventus-env/llvm`
  * 功能仿真器 `./ventus-env/spike` （对本项目有参考价值）
  * 周期仿真器 `./ventus-env/cyclesim` （对本项目有参考价值）
  * Chisel RTL 及其仿真器 `./ventus-env/gpgpu`
  * OpenCL 实现 `./ventus-env/pocl` （基于 PoCL 开源项目改造，Ventus相关主要在 `lib/CL/lib/CL/devices/ventus`）
  * Ventus 驱动程序 `./ventus-env/driver`
本项目不要直接引用 ventus-env/ 中的源码，如果本项目需要，将其复制到本项目。
不要修改 ventus-env/ 中的文件，如果本项目需要，将其复制到本项目后修改。
仅在做将本项目集成进 Ventus 工具链时才可能需要修改 ventus-env，这时需要请求用户同意，且一般只需要修改 `ventus-env/driver/driver/ptx_device`

使用 Ventus ELF 作为输入，目前阶段采用（不要使用 .vmem 文件）：
* `ventus-env/rodinia/opencl/*/*.riscv`
* `ventus-env/pocl/build/examples/*/*.riscv`

## tools/ 脚本约束

如果需要新增额外工具脚本，必须放在 `tools/` 路径下，并满足“单文件可维护性”要求：
* 脚本文件头必须自带四段说明：`背景`、`需求/作用`、`用法`、`实现原理/处理步骤`
* 关键处理步骤需添加简要注释（避免无上下文的黑盒逻辑）
* 设计上应尽量做到“仅阅读该单文件即可理解并维护”

## 文档同步约束

做任何代码/脚本/配置变更后，必须同步检查并更新相关文档（至少包括 `README.md`、`doc/` 下对应文档、必要时 `tools/README.md`）。

使用 OpenSpec 开展工作时，必须将“检查/修改文档”显式纳入执行计划，并在收尾时完成一致性校对。
