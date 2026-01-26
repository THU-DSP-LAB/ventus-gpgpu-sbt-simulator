乘影(Ventus)开源GPGPU项目是一个基于RISC-V及其V扩展指令集的学术界GPGPU项目    
包含指令集定义、软件栈与编译器实现、Chisel RTL与仿真器实现等    
编程模型（SIMT）与底层硬件结构与NVIDIA GPU有相似之处，本仓库希望尝试将乘影GPGPU的指令做静态二进制翻译为PTX指令，从而在NVIDIA GPU上做快速的功能仿真

乘影Ventus的指令集(ISA)存在两部分：标量部分与向量部分
* 标量：主要基于RV32IMA_zicsr_zfinx，这类似于NVIDIA SASS中的Uniform datapath(per-warp)
* 向量：主要基于RISC-V V扩展，但经过重解释以适配GPGPU的需求。类似于NVIDIA SASS中per-thread的普通指令
* 额外增加了一些标量/向量的Custom指令以适配GPGPU的需求

Ventus ISA向量部分对RVV指令语义的修改（待完善）
* 基本不使用RVV的访存指令
* 不使用RVV的mask机制，v0寄存器是普通向量寄存器。使用自定义的vbranch系列（例如vbeq, vbne等）指令、setrpc指令、join指令实现基于SIMT stack的warp分支管理

项目文件结构（目前项目刚刚开始，文件数量很少）
* README.nd, AGENTS.md
* `VEntusInst_basic.xlsx` 是Ventus ISA的基本指令表。第A列标注了指令的分类例如Custom/RV32I/M/V等；第B~AG列是指令的二进制格式；第AH列是指令的汇编格式；第AI列是指令的简单描述；第AJ列是备注信息；第AK列的yes/no标识本项目目前是否需要考虑此指令，标注no的行直接可忽略。
  * 文件中存在较多的合并单元格，读取时需要注意
  * Custom指令描述比较详细，RISC-V官方定义的指令如果没有重解释可能只在第AH列列出指令名


设想feature：
* “用户态”仿真：不支持多虚拟地址空间
* device ABI兼容：不需要对现有软件栈做太多修改，直接兼容原有host-devie约定（指令语义 + kernel meta (CTA调度器接口+metadata buffer, 这些信息稍后大都保存在CSR中) + 内存视图）
  * 必要的kernel meta 采用 PTX kernel param 形式传递，ventus kernel args 被内嵌于 CSR_KNL 指向的地址中按照 `_start` 中的方案取出即可
  * 库函数采用同语义替换（`_start`, `get_global_id`等）


项目进度: 当前处于原理验证阶段，在 `lab/` 目录下做一些 idea 有效性验证实验
