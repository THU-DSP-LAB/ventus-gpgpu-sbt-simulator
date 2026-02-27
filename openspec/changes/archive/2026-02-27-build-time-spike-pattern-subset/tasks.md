## Implementation Tasks

- [x] 1. CMake: 添加构建期输入参数 `SBT_SPIKE_ENCODING_H`（cache PATH），默认指向 `${CMAKE_SOURCE_DIR}/../spike/riscv/encoding.h`；若不存在则 `FATAL_ERROR` 并提示如何传参
- [x] 2. CMake: 增加 build-time codegen 的 `add_custom_command(OUTPUT ...)`，用 `gen_spike_encoding_subset` 生成 `${CMAKE_BINARY_DIR}/generated/spike_encoding_subset.hpp`
- [x] 3. CMake: 正确声明依赖关系（`DEPENDS` 包含 `data/spike_want.txt`、`${SBT_SPIKE_ENCODING_H}`、以及 `gen_spike_encoding_subset` target），确保输入变更触发重新生成与重编译
- [x] 4. CMake: 为 `sbt_decode`/`sbt_ptx` 增加 include 路径（`${CMAKE_BINARY_DIR}/generated`）并增加对 codegen target 的构建依赖
- [x] 5. `tools/sbt_decode.cpp`: 移除运行期 `encoding.h` 解析与 want 文件加载逻辑，改为 `#include "spike_encoding_subset.hpp"` 并直接构造 `patterns`
- [x] 6. `tools/sbt_decode.cpp`: 删除 `--encoding-h` 参数与 usage 文档，确保 CLI 不再暴露 runtime override
- [x] 7. `tools/sbt_ptx.cpp`: 同步改为使用 build-time 生成的 subset patterns；移除运行期 `encoding.h` 解析与 want 文件加载
- [x] 8. `tools/sbt_ptx.cpp`: 删除 `--encoding-h` 参数与相关 cache key 字段（`encoding_h_abs/encoding_time`），避免死代码与无效缓存比较
- [x] 9. `tools/gen_spike_encoding_subset.cpp`: 调整为在构建期可被稳定调用（确保 want 路径由构建系统显式指定；生成失败时清晰报错缺失的 want id）
- [x] 10. `tools/check_spike_want_consistency.sh`: 更新为匹配新流程（不再依赖 `GPU_SBT_WANT_FILE`/`--encoding-h`；继续覆盖一次 `gen_spike_encoding_subset` + `sbt_decode` + `sbt_ptx` 的最小一致性 smoke）
- [x] 11. 文档同步：
- [x] 11.1 [README.md](/work/ventus-env/sbtsim/README.md)：移除 `--encoding-h` 用法；新增构建期参数 `SBT_SPIKE_ENCODING_H` 说明
- [x] 11.2 [doc/IMPLEMENTATION_CODEMAP.md](/work/ventus-env/sbtsim/doc/IMPLEMENTATION_CODEMAP.md)：更新“pattern 来源”描述为 build-time subset header
- [x] 11.3 [doc/IMPROVEMENT_PROPOSALS.md](/work/ventus-env/sbtsim/doc/IMPROVEMENT_PROPOSALS.md)：同步修正 1.1 的“遗留问题”描述（不再是运行期解析）
- [x] 12. 验证：
- [x] 12.1 `cmake -S . -B build && cmake --build build -j` 通过（在 `SBT_SPIKE_ENCODING_H` 缺失时应明确失败）
- [x] 12.2 `tools/regress.sh --preset quick --arch sm_75` 通过
- [x] 12.3 行为一致性检查：确认 `sbt_decode/sbt_ptx` 不再引用 `GPU_SBT_WANT_FILE` 或尝试读取 `encoding.h`（可通过代码检索或 `strace -e openat` 佐证）

- [x] 13. `ventus-env` 集成：更新 `driver/driver/ptx_device/ventus.cpp`，移除 `GPU_SBT_WANT_FILE`/`GPU_SBT_ENCODING_H` 与 `sbt_ptx --encoding-h` 依赖，使其兼容“patterns 构建期固化”的新 `sbt_ptx`
- [x] 14. `ventus-env` 集成：更新 `build-ventus.sh` 的 `build_ptxsim()`：显式传 `-DSBT_SPIKE_ENCODING_H=${SPIKE_DIR}/riscv/encoding.h`，并移除安装运行期 want/encoding 数据的逻辑
