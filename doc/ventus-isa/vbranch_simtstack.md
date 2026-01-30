分支指令行为
线程束分支：部分线程执行if路径，其它线程执行else路径
由如下自定义指令配合完成：
1. setrpc：写入CSR[RPC]的值
2. vbranch系列（如vbeq等）：计算新分支两路径mask、计算跳转目的PC、压栈simt stack两次、跳转
  1. 在current mask（此vbranch未完成前的mask）的基础上，计算两个新分支路径各自的mask。若某个新分支mask全0，则直接跳转到另一个分支，不做任何其他操作（不压栈）
  2. 两个新分支的起始PC分别为：pc+4、pc+offset
    1. 线程数较少的分支(在本文档中)称为PATH1，先执行
    2. 线程数较多的分支(在本文档中)称为PATH2，后执行
  3. 压栈simt stack
    - 字段名 | ==   RPC    == | ==  New PC  == | ==   New Mask   == |
    - 先压入 | == CSR[RPC] == | == CSR[RPC] == | == current mask == |
    - 再压入 | == CSR[RPC] == | == PATH2 PC == | == PATH2   mask == |
3. join：弹栈simt stack一次、跳转
  1. 若当前PC等于栈顶RPC字段值，则：跳转到栈顶New PC字段值处，mask设置为栈顶New Mask字段值，并弹栈。
  2. 否则（PC不等于栈顶RPC，或栈空），此join指令不做任何操作

举例：一个简单的vbranch分支
- 先setrpc，RPC处总为此branch的join指令
- 再vbranch，压栈两次，跳转（或顺序进入）到PATH 1
- PATH 1需自行抵达RPC处的join指令（可能是顺序抵达，也可能是分支末无条件跳转去）
- 第一次执行join，PC等于栈顶RPC，因此：加载PATH2的Mask，跳转到PATH2的起始PC
- PATH2需自行抵达RPC处的join指令（可能是顺序抵达，也可能是分支末无条件跳转去）
- 第二次执行join，PC等于栈顶RPC，因此：成功恢复分支前的Mask，跳转New PC字段值，但此值必定等于此join指令地址
- 第三次执行join：此分支行为彻底完成
  - 若栈空（上述过程为最外层的分支）：不做任何操作，顺序执行下一条指令
  - 若栈不空：上述过程为嵌套的内层分支
    - 若PC不等于栈顶RPC（更外层分支的此PATH尚未抵达汇聚点）：不做任何操作，顺序执行下一条指令
    - 若PC等于栈顶RPC（更外层分支的此PATH已抵达汇聚点，也就是说嵌套的分支共享了相同的汇聚点）：更新PC = New PC，Mask = New Mask，弹栈
      - 根据新PC值，可能执行外层分支的PATH2，也可能因外层分支也已汇聚从而第四次执行此join以判定是否有再外层的共享此join的分支（直到栈空或PC不等于栈顶RPC为止）

SIMT stack的深度 = warp_size = 32