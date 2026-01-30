* regext 指令是“前缀指令”，它只作用于下**一条**指令，它和它的下一条指令可以被认为是捆绑到一起作为一条大指令。其 12-bit imm 被划分为 ext-s1, ext-s2, ext-s3, ext-d 四个字段，每个 3-bit，分别拼接到下一条指令的对应字段，将下一条指令的 s1, s2, s3, d 字段从 5-bit 扩展到 8-bit 从而支持 256 个寄存器
* regexti 同样只作用于下**一条**指令。其详细作用待补充说明。此指令基本不出现，现阶段可忽视之
* Ventus目前不使用RVV的mask机制，v0为普通向量寄存器，RVV中使用vm的指令其vm字段相当于无效，mask由SIMT stack管理
* 访存（例如 vlw12/vsw12）暂且认为必须对齐访存粒度，例如 vlw12/vsw12 需要4字节对齐（TODO: 待确认）
* 除零、浮点误差等行为在当前阶段暂时忽略 corner case（TODO）
* `barrier` 与 CUDA `__syncthreads()` 完全等同。ventus要求不能在发散路径上执行 `barrier`

地址空间
* `0x7000_0000` ~ `0x7fff_ffff` 为 shared memory （或OpenCL术语中的local memory）对应到 SM 内的 SRAM，其他地址空间都对应显存 DRAM。每一个线程块的 shared memory 基地址有所不同，必须从 `CSR_LDS` 获取
* `0x8000_0000` ~ `0x8fff_ffff` 为存储指令与部分编译器嵌入的静态数据
* 其他空间为 global memory，而 CUDA local memory 是 global memory 中 allocate 出来的一段空间
* 目前并没有显式的 Constant memory 段
