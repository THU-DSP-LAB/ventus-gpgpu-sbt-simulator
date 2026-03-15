## Implementation Tasks

- [ ] 1. 在 `sbt/ptx_emit.cpp` 引入新的 leader-lane 基础语义辅助接口，至少包括：
  - 当前 `leader_lane` 的单一值来源
  - use-point `activemask` 读取
  - use-point `laneid == leader_lane` 谓词生成
  - leader-to-all-lane 标量广播 helper
  - 明确 kernel entry 初始 leader 的建立规则，以及 helper entry 从 call-state 继承 `leader_lane` 的恢复顺序

- [ ] 2. 建立 leader-lane `x-reg` canonical state 路径：
  - 让函数体内标量 `x-reg` 真值从 shared `WarpCtx` 迁移到 leader-lane PTX scalar regs
  - 明确 `x0`、leader-only 标量 ALU、leader-only 标量 memory/CSR 更新的实现路径
  - 对仍暂时存活的旧 shared 标量路径保留必要同步，避免迁移期误删正确性依赖

- [ ] 3. 实现 structured divergence 的 leader / scalar-state 迁移协议：
  - 在 `vbranch` 入口为每条非空路径选取 path-local leader
  - 在路径开始执行前完成 branch-entry `x-state` 到 path-local leader 的 handoff
  - 在每个 `join` 前驱边尾部实现路径最终 scalar state 的 reconverge 协议
  - 在 `join` 后重新选择当前 leader，并覆盖 nested `vbranch/join` 与 shared-join 情况
  - 若无法可靠识别 predecessor-edge 放置点或无法维持当前 uniformity contract，显式 fail-fast 而不是继续发射 PTX

- [ ] 4. 设计并落地 direct-call value ABI 的 emitter 路径：
  - 定义 `Mutable CallState` / `ReadOnly MachineContext` / `ReadOnly RuntimeEnv` 的 PTX 参数形状
  - 明确 `active mask` / reconvergence 等 implicit exec state 不进入 value ABI
  - 让 call 边界显式传递 `leader_lane`、logical `x-reg`、logical `v-reg` 与所需可变 CSR
  - 明确 scalar marshalling / unmarshalling 以当前 leader 所持有的 canonical `x-state` 为 authoritative 来源，而不是把 call 当成 all-lane scalar broadcast use
  - 让 divergence-path 内 direct call 使用当前 path-local call state
  - 让 callee 在任何 Ventus 标量相关操作前先恢复 `leader_lane` 与 canonical scalar state
  - 用新 value ABI 替换当前 `.local vctx` spill / restore 主线路径
  - 同步更新 helper prototype emission，使 prototype / definition 与新 value ABI 保持一致

- [ ] 5. 在 leader 合法性边界已经落地后，重构 block 入口 leader derivation 路径：
  - 仅在 structured divergence 与 direct call 边界已能建立合法 `leader_lane` 的路径上，移除“每个 basic block 固定发射 `activemask/bfind/setp`”的主线规则
  - 将现有 leader 相关 lowering 改为 use-point 派生
  - 审查 call / divergence / join 边界，确保不会复用 stale 的 mask / predicate

- [ ] 6. 实现 all-lane scalar consumer 的统一广播规则：
  - 先完成标量条件分支 `beq/bne/blt/bge/bltu/bgeu` 的 leader-to-all-lane 操作数广播，再继续使用 `bra.uni`
  - 从当前 `sbt/ptx_emit.cpp` 反推 all-lane scalar consumer inventory，并在设计/注释/验证材料中固化这份闭表
  - 枚举并修复其它 all-lane 消费标量值的 lowering，至少覆盖现有 `vmv_v_x` / `vmv_s_x` / `vfmv_v_f`、`vmerge_vxm` / `vfmerge_vfm`、`vadd_vx` 与同类 `vx` 路径
  - 将当前 inventory 覆盖到的这些 use-site 强制收敛到显式 broadcast helper 入口，避免继续直接读取“所有 lanes 已本地一致”的旧语义
  - 明确哪些路径仍保持 leader-only，哪些路径属于 all-lane scalar consumer

- [ ] 7. 处理 staged coexistence 与旧路径退场：
  - 明确新旧路径共存期间哪些 call / scalar / divergence lowering 仍走旧实现
  - 禁止 silent fallback；新路径未覆盖的情况应明确 fail-fast
  - 在验证覆盖足够后，移除旧 `WarpCtx.x[]` canonical 语义和旧 `.local vctx` 主线路径

- [ ] 8. 增加针对本 change 的验证与回归：
  - 增加或扩展 microtests，覆盖 leader-lane scalar ALU、scalar branch broadcast、direct call、divergence-path call、nested `vbranch/join`
  - 增加 shared-join 场景测试，准备能暴露 `vbranch` 入口 leader handoff、predecessor-edge reconverge 与 `join` 后 leader 重选错误的用例
  - 增加“all-lane scalar consumer inventory 与实际 lowering use-site 对齐”的审查项或回归
  - 对 compile-first 与语义对照路径执行可复现验证；后端单测/回归命令遵守仓库超时约束

- [ ] 9. 同步更新公共文档与导航：
  - 更新 `README.md`、`doc/README.md`、`doc/IMPLEMENTATION_CODEMAP.md`，说明新的 leader-lane scalar state / value ABI / branch broadcast 主线
  - 统一公共文档中的 `leader lane` 术语，移除旧 `owner lane` 表述
  - 根据实现落地情况决定是否更新或归档现有 PTX 设计文档，避免与新主线口径冲突
