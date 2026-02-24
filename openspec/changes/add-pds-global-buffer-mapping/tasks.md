## 1. PTX ABI wiring (PDS params)

- [ ] 1.1 扩展 PTX `.entry` 参数列表：新增 `pds_base_vaddr` 与 `pds_size_per_thread`（u32）
- [ ] 1.2 更新 `ventus-env/driver/driver/ptx_device/ventus.cpp` 的 `cuLaunchKernel` 参数列表，传入 `vt_kernel_metadata_t::{pdsBaseAddr,pdsSize}`
- [ ] 1.3 更新 PTX `.func` 直调（call）参数透传：确保被调函数也能获得 PDS 参数（如需要）

## 2. CSR_PDS compute

- [ ] 2.1 在 PTX emitter 中实现 `CSR_PDS(0x807)` 的 `csrrs` 读取：按 spec 的 block/warp 公式计算并写回 x-reg
- [ ] 2.2 增加 fail-fast 校验：`pds_base_vaddr` 必须位于 heap/global 数值地址区间（否则报错退出）

## 3. `vlw.v/vsw.v` lowering (global PDS buffer)

- [ ] 3.1 修正 `vsw.v` 的立即数解码宽度/符号扩展（与 simm11 语义对齐）
- [ ] 3.2 将 `vlw.v/vsw.v` lowering 改为：计算 PDS 数值地址后走通用地址映射 `ld/st.global`（不再使用 `ld/st.local`）
- [ ] 3.3 移除 `.local __sbt_pds[]` 临时模拟路径及相关选项/环境变量依赖

## 4. Regression (no new diagnostic tooling)

- [ ] 4.1 `tools/rodinia_ptx_smoke.sh` compile-first 不回退
- [ ] 4.2 至少新增/确认一个实际触发 `vlw.v/vsw.v` 的端到端用例在 PTX backend 下可运行

