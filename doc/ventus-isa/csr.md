| 名称        | 地址    | 描述                                                                          |
|-----------|-------|-----------------------------------------------------------------------------|
| CSR_TID   | 0x800 | 该 warp 中 id 最小的 thread id，其值为 CSR_WID * CSR_NUMT，配合 vid.v 可计算其它 thread id   |
| CSR_NUMW  | 0x801 | 该 workgroup 中的 warp 总数                                                      |
| CSR_NUMT  | 0x802 | 一个 warp 中的 thread 总数                                                        |
| CSR_KNL   | 0x803 | 该 workgroup 的 metadata buffer 的 baseaddr                                    |
| CSR_WGID  | 0x804 | 该 SM 中本 warp 对应的 workgroup id                                               |
| CSR_WID   | 0x805 | 该 workgroup 中本 warp 对应的 warp id                                             |
| CSR_LDS   | 0x806 | 该 workgroup 分配的 local memory 的 baseaddr，同时也是该 warp 的 xgpr spill stack 基址    |
| CSR_PDS   | 0x807 | 该驻留 workgroup PDS 槽位的 private memory baseaddr；私有向量指令用 CSR_NUMW/CSR_NUMT/CSR_TID 做 workgroup 级交错寻址 |
| CSR_GDX   | 0x808 | 该 workgroup 在 NDRange 中的 x id                                               |
| CSR_GDY   | 0x809 | 该 workgroup 在 NDRange 中的 y id                                               |
| CSR_GDZ   | 0x80a | 该 workgroup 在 NDRange 中的 z id                                               |
| CSR_PRINT | 0x80b | 向 print buffer 打印时用于与 host 交互的 CSR                                          |
| CSR_RPC   | 0x80c | 重汇聚 pc 寄存器 (Rereconvergence PC)                                                          |

注：
* 这里采用了OpenCL风格的术语（warp除外）
* 目前除了 `CSR_RPC` 会被 `setrpc` 指令修改并影响 SIMT stack之外，其他所有 CSR 对程序都是只读的，也不存在调度/屏蔽等副作用
* 上述 CSR 全部都是标量的（warp-uniform，而非 per-thread）
* 在本项目的原型验证阶段，基本可以忽略 RISC-V 原生的 CSR 及其副作用，例如 mstatus 和 RVV 定义的 CSR 等
