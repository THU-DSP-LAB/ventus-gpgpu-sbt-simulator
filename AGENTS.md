<!-- OPENSPEC:START -->
# OpenSpec Instructions

These instructions are for AI assistants working in this project.

Always open `@/openspec/AGENTS.md` when the request:
- Mentions planning or proposals (words like proposal, spec, change, plan)
- Introduces new capabilities, breaking changes, architecture shifts, or big performance/security work
- Sounds ambiguous and you need the authoritative spec before coding

Use `@/openspec/AGENTS.md` to learn:
- How to create and apply change proposals
- Spec format and conventions
- Project structure and guidelines

Keep this managed block so 'openspec update' can refresh the instructions.

<!-- OPENSPEC:END -->

@README.md

codex cli中的AI助手受到沙箱限制，如果需要运行程序检测/使用GPU，可能需要申请突破沙箱权限

**临时**：已将整个ventus项目（工具链、仿真器、测例等）放在 (ventus-env)[./ventus-env] 供当前阶段参考
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
仅在做将本项目集成进 Ventus 工具链时才可能需要修改 ventus-env，这时需要请求用户同意，且一般只需要修改 `ventus-env/driver/driver/ptx-device`

使用 Ventus ELF 作为输入，目前阶段采用（不要使用 .vmem 文件）：
* `ventus-env/rodinia/opencl/*/*.riscv`
* `ventus-env/pocl/build/examples/*/*.riscv`
