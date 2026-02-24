# Change: Refactor instruction whitelist to single source + extend instruction support

## Why
当前工程在“Ventus 扩展指令 pattern 白名单（bring-up whitelist）”上存在多处重复列表（`sbt_ptx` / `sbt_decode` / `gen_spike_encoding_subset`），且内容不一致会造成：
- 同一份输入在不同工具路径下出现 “decode 能过但 emit 不支持 / 或相反” 的隐性分叉；
- 新增/修复指令支持时需要多点同步修改，维护成本高、容易漏改。

同时，PTX emitter 对 RV32 标量与部分 RVV 风格指令的支持仍不完整，限制了可翻译输入范围。

## What Changes
- 引入 **单一事实源** 的 whitelist 配置文件，所有工具只从该配置加载需要的 Spike `DECLARE_INSN` ID 集合（下划线命名）。
- 移除代码中重复/分散的 `want` 列表，消除不一致来源。
- 在不引入新的诊断/扫描工具、不新增“缺失指令名列表输出”的前提下，按“指令族”补齐 PTX emitter 的指令覆盖（重点先补齐：RV32I/M/zicsr 常用子集，以及 Ventus 基础表中缺失的向量访存与常用算术/逻辑指令）。

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

