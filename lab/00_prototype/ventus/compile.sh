source ~/ventus-env/env.sh
/home/wangyh/ventus-env/install/bin/clang -target riscv32 -mcpu=ventus-gpgpu simple.S -o simple.riscv -nodefaultlibs -O1 -Wl,-T,/home/wangyh/ventus-env/install/lib/ldscripts/ventus/elf32lriscv.ld -Wl,--init=simple -w 
/home/wangyh/ventus-env/install/lib/scripts/assemble.sh simple 
