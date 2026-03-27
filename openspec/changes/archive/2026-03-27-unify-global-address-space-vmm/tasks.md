## Implementation Tasks

- [x] 1. 在 `driver/driver/ptx_device/ventus.cpp` 建立单一 `Global` VMM backing 基础设施
  - 启动时检测 `CU_DEVICE_ATTRIBUTE_VIRTUAL_ADDRESS_MANAGEMENT_SUPPORTED`
  - 用 `cuMemAddressReserve` 保留逻辑 `Global` 区域
  - 定义 granularity-aware 的 mapped-range / handle bookkeeping，并在不支持 VMM 时显式报错退出

- [x] 2. 把 driver 的 ELF 上传、普通分配、复制与释放路径迁移到单一 `Global` 空间
  - 让 ELF PT_LOAD 上传映射到 `global_base + (vaddr - global_base_vaddr)`
  - 让 `vt_buf_alloc`、`vt_copy_to_dev`、`vt_copy_from_dev`、PDS bitmap / metadata / pool 管理都基于单一 `Global` backing
  - 删除运行期 `elf_base` / `heap_base` 双路由逻辑

- [x] 3. 重构 `sbt/ptx_emit.{hpp,cpp}` 的普通地址空间 lowering 与 kernel entry ABI
  - 将 options 从 `shared_base_vaddr + elf_base_vaddr + heap_base_vaddr` 收敛为 `shared_base_vaddr + global_base_vaddr`
  - 把普通 load/store lowering 改成只区分 `Shared` / `Global`
  - 将 kernel entry 参数从 `elf_base, heap_base, ...` 改成 `global_base, ...`
  - 保持 invalid address fail-fast，不引入静默 fallback

- [x] 4. 更新 direct-call runtime environment、helper prototype/definition 与相关测试
  - 将 `runtime_env_blob` 从 `elf_base + heap_base` 收敛为单一 `global_base`
  - 更新 helper prototype emission、definition signature、marshal / unmarshal
  - 同步修正 `ptx_emit_call_prototype_test`、`ptx_emit_leader_lane_abi_test` 及其他 ABI 相关断言

- [x] 5. 重新校正 PDS 和普通 Global 访问的 current contract 与验证点
  - 确保 PDS 仍作为 `Global` 子区域工作，而不是新的普通地址空间类别
  - 校对 `pds_base_vaddr`、bitmap、metadata、arg buffer 在单一 `Global` 模型下的地址映射与 fail-fast 规则
  - 运行受影响的 compile-first / smoke / regression 验证

- [x] 6. 同步更新长期文档与 OpenSpec current 索引
  - 更新 `README.md`
  - 更新 `doc/README.md`
  - 更新 `doc/IMPLEMENTATION_CODEMAP.md`
  - 更新 `doc/ADDRESS_SPACE_SPECIALIZATION.md`
  - 更新 `openspec/README.md`
  - 将本 change 的 delta 同步到相关 current specs

- [x] 7. 做最终一致性检查
  - 确认代码、README、`doc/`、`openspec/specs/`、`openspec/changes/unify-global-address-space-vmm/` 对 `Shared + Global` 的口径一致
  - 确认 `current` / `active` / `historical` / `legacy` 标签与索引没有冲突
  - 确认仓库中不再把 `ELF/Heap` 写成当前 PTX 普通访存的运行期分流 contract
