## Why

当前 `sbt_decode` / `sbt_ptx` 在运行期需要：
- 读取 want 白名单（默认 `data/spike_want.txt`，可被 `GPU_SBT_WANT_FILE` 覆盖）
- 解析 Spike 的 `encoding.h`（默认 `../spike/riscv/encoding.h`，可用 `--encoding-h` 覆盖）

这类“运行期外部文件依赖 + 运行期可覆盖入口”带来的问题很现实：
- 可复现性差：同一份二进制在不同目录结构/环境变量下可能产生不同的解码/支持范围。
- 调试成本高：pattern 的实际来源与版本不易追溯；同时 `sbt_ptx` 的 cache key 目前不包含 want 文件变更，容易出现“改了 want 但命中旧缓存”的误判。
- 集成成本高：在 PoCL/driver 端到端链路中，运行时找不到 `encoding.h` 或 want 文件会变成额外的环境失败点。

本变更选择把 Spike pattern 子集改为 **build-time codegen 并固化到二进制**，并且 **完全不保留运行期可覆盖入口**，换取可审阅、可复现、易集成的行为。

## What Changes

- 引入 build-time codegen：在构建期生成 Spike pattern 子集头文件（subset header）
  - 输入：仓库内 `data/spike_want.txt` + 指定的 Spike `encoding.h`
  - 输出：一个可被编译单元直接 `#include` 的头文件（位于 build 目录的 generated 路径），内容为 `constexpr` `{name, match, mask}` 数组
- 修改 `sbt_decode` / `sbt_ptx` 的 pattern 构造方式：不再在运行期读取 want/解析 `encoding.h`，改为直接使用编译进二进制的 subset patterns
- 移除所有运行期覆盖入口（针对 `sbt_decode` / `sbt_ptx`）：
  - 不再读取 `GPU_SBT_WANT_FILE`
  - 不再提供/接受 `--encoding-h` 作为运行期可变输入
- 构建系统显式追踪输入依赖：want 或 `encoding.h` 改动会触发重新生成与重编译；若构建期缺失 `encoding.h`，构建直接失败（不做静默兜底）

## Capabilities

### New Capabilities
- **Hermetic patterns at runtime**：`sbt_decode` / `sbt_ptx` 运行时无需依赖外部 want/`encoding.h` 文件即可完成 Ventus 扩展指令解码。

### Modified Capabilities
- **inst-support**：Spike pattern 来源从“运行期解析 + want 文件”调整为“构建期生成并固化的 subset header”，并去除运行期可覆盖入口。

## Impact

- 破坏性变化（预期且明确）：
  - 运行期不再允许通过环境变量或命令行参数切换 want/`encoding.h`；想改变 pattern 子集只能改输入文件并重新构建二进制。
- 正向收益：
  - 回归与端到端集成更稳定：运行时不再依赖 `ventus-env/spike` 目录结构，减少环境类失败。
  - 可复现性更强：同一仓库状态构建出的二进制具有固定的 pattern 集合；支持范围更易审阅与追溯。
- 构建要求：
  - 构建环境必须提供 Spike `encoding.h` 的可用路径（默认按 ventus-env 子项目布局查找，必要时通过 CMake cache 参数显式指定）。

