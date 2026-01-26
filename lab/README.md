当前项目处于原理验证阶段，在本路径下做一些简单的实验验证idea的可行性

本路径下做完的实验，列一句简单的描述在下边

阶段0：手工转换 ventus isa -> ptx
* `00_prototype`：极简易的手动转换 ventus isa -> ptx 完成单warp向量加法
* `01_host_device_abi`：手工实现 ventus kernel ABI（CSR_KNL metadata + arg buffer）的PTX并在NVIDIA GPU运行 vecadd，主要目的是探究能否兼容Ventus host-device API/ABI

阶段1：集成到现有工具链
* `02_host_device_abi_integrate`：在 ventus-env 的 driver 中新增 `ptx_device` 后端，通过 PoCL 端到端运行 vecadd（`VENTUS_BACKEND=ptx`），实现“无需改 PoCL 上层代码”的真实验证