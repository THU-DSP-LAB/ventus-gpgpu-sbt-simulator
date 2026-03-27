@README.md

## Canonical Docs

- 用户入口与命令：`README.md`
- 当前实现真相：`doc/IMPLEMENTATION_CODEMAP.md`
- OpenSpec 分层与当前 spec 入口：`openspec/README.md`
- 当前 contract：`openspec/specs/`
- 历史材料：`doc/archive/`、`lab/`、legacy specs

除非文档明确写着 `current` / `active`，否则不要把历史设计稿、回归报告、实验目录当作当前实现口径。

## Repo Context

本仓库作为 `ventus-env` 的子项目集成使用时，`ventus-env` 位于本仓库的上级目录（`../`）。
* ventus 有关的软件工具使用 `../install` 目录下的可执行程序或库，通过 `source ../env.sh` 设置环境变量让 OpenCL App 在 ventus 环境中运行
* ventus 编译器：`../install/bin/clang -cl-std=CL2.0 -target riscv32 -mcpu=ventus-gpgpu kernel.cl -o kernel.riscv -nodefaultlibs -Wl,${VENTUS_ENV_PATH}/install/lib/crt0.o -Wl,${VENTUS_ENV_PATH}/install/lib/riscv32clc.o -Wl,--gc-sections -L${VENTUS_ENV_PATH}/install/lib -lworkitem -I${VENTUS_ENV_PATH}/installinclude/clc -O1 -Wl,-T,${VENTUS_ENV_PATH}/install/lib/ldscripts/ventus/elf32lriscv.ld -Wl,--init=${KERNEL_FUNC_NAME} -w -D__opencl_c_generic_address_space=1 -D__opencl_c_named_address_space_builtins=1 -D__OPENCL_VERSION__=200` 注意替换 `${VENTUS_ENV_PATH}` 和 `${KERNEL_FUNC_NAME}`
* ventus 反汇编器：`../install/bin/llvm-objdump -d --mattr=+v,+zfinx kernel.riscv > kernel.dump`
* `ventus-env` 典型子仓库包括：
  * 编译器 `../llvm`
  * 功能仿真器 `../spike`
  * 周期仿真器 `../cyclesim`
  * Chisel RTL 及其仿真器 `../gpgpu`
  * OpenCL 实现 `../pocl`
  * Ventus 驱动程序 `../driver`

本项目通常不应直接修改上级 `ventus-env` 的源码；仅在做“集成到工具链”的工作（例如 `driver/driver/ptx_device`）时例外。
* 如果修改了 driver / pocl 等其它 ventus-env 子项目源码，需用 `../build-ventus.sh` 来编译安装新版本到 `../install` 或者其它临时目录
* 临时安装目录：推荐 `cp -a --reflink=auto` 复制 `../install` 然后单独按需编译安装修改的子项目，仿照 `../env.sh` 重置环境变量

使用 Ventus ELF 作为输入，采用（不要使用 `.vmem` 文件）：
* `../rodinia/opencl/*/*.riscv`
* `../pocl/build/examples/*/*.riscv`

## tools/ 约束

如果需要新增额外工具脚本/程序/库，必须放在 `tools/` 路径下。
`tools/` 路径下的所有文件满足“单文件可维护性”要求：
* 脚本文件头必须自带四段说明：`背景`、`需求/作用`、`用法`、`实现原理/处理步骤`
* 关键处理步骤需添加简要注释（避免无上下文的黑盒逻辑）
* 设计上应尽量做到“仅阅读该单文件即可理解并维护”
* 如果单一工具需要多文件实现，给一个入口脚本 + 同名子路径下的具体实现，在入口脚本维护上述四段说明，具体实现中按需添加简明注释

## 文档同步约束

做任何代码/脚本/配置变更后，必须同步检查并更新相关文档（至少包括 `README.md`、`doc/` 下对应文档、必要时 `tools/README.md`）。

使用 OpenSpec 开展工作时，必须将“检查/修改文档”显式纳入执行计划，并在收尾时完成一致性校对。

文档修订时必须显式区分以下状态：
* `current`：当前实现真相 / 当前 contract
* `active`：仍在推进的规划或优化轨道
* `historical`：仅用于回溯背景
* `legacy`：保留参考价值，但已不再是当前入口或回归基线

文档分层规则：
* `doc/`：长期维护文档；允许 `current` 文档，以及少量跨多个 change 仍成立的长期 `active` 专题
* `openspec/changes/`：具体 change 的 proposal/design/tasks 与阶段性实施材料
* `doc/archive/`、`lab/`：历史文档与实验材料
* 同一主题在 `doc/` 根目录只保留一个 `active` 入口；不要把同一问题拆成多个并列 active 文档

若某份 proposal/design 已在前几次提交中落地，不允许继续把它写成未来时；必须同步更新状态说明、索引位置与引用口径。
