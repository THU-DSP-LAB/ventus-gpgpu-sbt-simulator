# Change: Refactor instruction whitelist to single source + extend instruction support

## Why
当前工程在“Ventus 扩展指令 pattern 白名单（bring-up whitelist）”上存在多处重复列表（`sbt_ptx` / `sbt_decode` / `gen_spike_encoding_subset`），且内容不一致会造成：
- 同一份输入在不同工具路径下出现 “decode 能过但 emit 不支持 / 或相反” 的隐性分叉；
- 新增/修复指令支持时需要多点同步修改，维护成本高、容易漏改。

同时，PTX emitter 对 RV32 标量与 RVV/Ventus 风格指令的支持仍不完整，距离 `VentusInst_basic.txt` 的基础指令表仍有明显差距，限制了可翻译输入范围与“通用性”。

## What Changes
- 引入 **单一事实源** 的 whitelist 配置文件，所有工具只从该配置加载需要的 Spike `DECLARE_INSN` ID 集合（下划线命名）。
- 移除代码中重复/分散的 `want` 列表，消除不一致来源。
- 以 `VentusInst_basic.txt` 为“目标指令集合”，分阶段补齐 decode 分类与 PTX lowering，力争覆盖其全部指令（允许明确列出少量例外：例如非 `ret` 形态的 `jalr` 等仍保持直接报错策略）。
- 引入面向扩展的回归方式：用 OpenCL 主机端创建输入/输出 buffer（A/B），让 device 程序把结果写入 B 并由主机读回；以 Spike（Ventus PoCL 设备）作为语义 oracle，与当前 PTX 路径在主机端对比输出（浮点允许近似、无需 bit-accurate）。

## Impact
- Affected areas:
  - whitelist/pattern 加载路径（`tools/` 与 `sbt/`）
  - PTX emitter 覆盖范围（`sbt/ptx_emit.*`）
  - （可选）生成 Spike subset header 的工具（`tools/gen_spike_encoding_subset.cpp`）
- Expected outcome:
  - whitelist 维护点从 N 处收敛为 1 处；
  - 指令支持扩展以“可回归”的方式推进，减少因不一致导致的带外问题。

## Non-goals
- 不新增新的“ELF 全量扫描/报表”类诊断工具与子命令。
- 不要求在工具输出中额外打印“未支持指令的具体名字清单”（保持现有错误处理风格即可）。
- 不修改 `ventus-env/` 下任何文件。
