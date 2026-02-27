## Context

变更前工程的 Ventus 扩展指令解码依赖两类运行期输入：
- want 白名单（`data/spike_want.txt`，可被 `GPU_SBT_WANT_FILE` 覆盖）
- Spike `encoding.h`（默认 `../spike/riscv/encoding.h`，可通过 `sbt_decode/sbt_ptx --encoding-h` 覆盖）

`sbt_decode/sbt_ptx` 会在运行期解析 `encoding.h` 得到 `{name, match, mask}`，再按 want 过滤成 `patterns` 传给 `decode_text()`。

本变更的目标是将 patterns 变为 **构建期生成并固化到二进制**，并移除所有“运行期可覆盖入口”，使 `sbt_decode/sbt_ptx` 在任意 CWD 与环境变量下行为一致，且不再依赖 `ventus-env/spike` 的目录布局。

## Goals / Non-Goals

**Goals:**
- `sbt_decode` / `sbt_ptx` 运行期不再读取 want 文件或解析 `encoding.h`。
- patterns 子集在构建期生成，改动 want/`encoding.h` 必须通过重新构建来生效。
- 明确的 fail-fast：构建期缺 `encoding.h` 或 want id 不存在时直接构建失败并给出清晰错误。
- 构建系统正确建立依赖关系：want/`encoding.h` 变更触发 subset header 重新生成与相关目标重编译。

**Non-Goals:**
- 不改变 decode/CFG/emit 的语义与支持范围（除移除运行期覆盖入口）。
- 不引入新的运行期兜底逻辑或“自动猜测/搜索 encoding.h”的隐式规则。
- 不要求把 Spike `encoding.h` / want 列表提交进最终发布产物（只固化 subset patterns）。

## Decisions

### 1. Build-time codegen: CMake 自定义命令生成 subset header

在 CMake 构建阶段新增一个自定义命令（`add_custom_command(OUTPUT ...)`）：
- 输入依赖：
  - `${CMAKE_SOURCE_DIR}/data/spike_want.txt`
  - Spike `encoding.h`（通过一个 CMake cache 变量指定；默认按 `ventus-env` 子项目布局给出路径）
  - 生成器可执行文件：`$<TARGET_FILE:gen_spike_encoding_subset>`
- 输出：
  - `${CMAKE_BINARY_DIR}/generated/spike_encoding_subset.hpp`（仅在 build tree 内生成，避免污染 source tree）

`sbt_decode` / `sbt_ptx` 编译时 `#include` 该 header，运行期直接使用编译进二进制的 patterns。

**Alternatives considered:**
- 运行期继续解析 `encoding.h`，仅把 want 固化到二进制：拒绝。无法解决“运行期外部文件依赖”与目录布局耦合问题。
- 将生成 header 提交进仓库（vendor generated file）：拒绝。会引入“generated file 与输入来源漂移”的人工同步成本。

### 2. 生成器复用现有 `gen_spike_encoding_subset`，但构建期显式传参

构建期生成 header 复用现有工具 `gen_spike_encoding_subset`，并由 CMake 显式指定：
- `--encoding-h <abs/path/to/encoding.h>`
- `--want-file <abs/path/to/data/spike_want.txt>`
- `--out <abs/path/to/generated/header>`

want 文件路径在构建期固定为 `${CMAKE_SOURCE_DIR}/data/spike_want.txt`，通过 `--want-file` 显式传给生成器，避免依赖当前 CWD 或环境变量残留。

**Alternatives considered:**
- 在 CMake 中用纯文本处理解析 `encoding.h`：拒绝。复刻解析逻辑成本高且更难测试。
- 在 `sbt_lib` 内加入“编译期固定 patterns”模块：拒绝。会与生成器目标形成构建环依赖（生成器依赖 `sbt_lib`）。

### 3. 避免构建环：生成 header 仅被 tool 目标引用

为避免 `gen_spike_encoding_subset -> sbt_lib -> (generated header)` 的循环依赖：
- 生成的 subset header **不** 被 `sbt_lib` 编译单元包含
- 仅在 `tools/sbt_decode.cpp` 与 `tools/sbt_ptx.cpp` 中包含并构造 `std::vector<sbt::Pattern>`

这样构建顺序为：`sbt_lib` → `gen_spike_encoding_subset` → (generate header) → `sbt_decode/sbt_ptx`。

**Alternatives considered:**
- 新增 `sbt_patterns` 独立库（依赖 generated header，供 tools 复用）：可行但非必要；当前优先保持改动面小。

### 4. 移除运行期可覆盖入口（明确破坏性变更）

按 spec 要求移除以下入口，使 pattern 选择不可在运行期变化：
- `GPU_SBT_WANT_FILE`（`sbt_decode/sbt_ptx` 不再读取）
- `sbt_decode --encoding-h`
- `sbt_ptx --encoding-h`

同时更新相关文档与脚本（如 `tools/check_spike_want_consistency.sh`）以匹配新行为。

**Alternatives considered:**
- 保留 `--encoding-h` 作为调试后门但默认不用：拒绝。与“完全不保留运行期可覆盖入口”的约束冲突。

## Risks / Trade-offs

- 构建期依赖增强：构建机必须能访问 Spike `encoding.h`。这是预期取舍，换取运行期无依赖与集成稳定性。
- 变更影响面涉及 build + CLI：需要同步更新 README/doc/ 与回归脚本，否则会出现“用法/参数”不一致。
- patterns 固化后，快速试验新的 want/encoding 需要重新构建二进制；这降低了交互式调试的便利性，但符合本变更目标（可复现优先）。
