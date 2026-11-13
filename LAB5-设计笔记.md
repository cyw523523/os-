# Lab5：完整 exec 与 COW fork

学号：2024302111375。参数来自原有 kernel/course_sid.h：LAB5_REFCNT_SCHEME=1，LAB5_COW_BUDGET=143，LAB5_KILL_SEMANTICS=1。

## 1. 实现范围与入口

基于 lab4-submit（18410d8）完成增量改造；课程包合入提交为 232f0a4，标签 lab5-start。先完成 exec/ELF 与引用生命周期框架并通过 Lab3 回归，再启用 COW 共享、缺页和内核写入分离，最后验证单核与双核。

关键入口：

| 文件 / 函数 | 职责 |
|---|---|
| kernel/syscall.c：sys_exec | 安全读取程序名、argv 指针数组及参数字符串 |
| kernel/proc.c：load / proc_exec | 载入内嵌 ELF、构造用户参数栈、提交新映像 |
| kernel/vm.c：uvmcopy | 父子用户页共享；可写页清 W、置 COW |
| kernel/trap.c：usertrap | scause=15 分派 COW，失败终止故障进程 |
| kernel/vm.c：cow_resolve | 独占恢复写权限或分配复制、发布映射、丢弃旧引用 |
| kernel/vm.c：usercheck / usercopy | 校验合法 COW 缓冲区；内核写入前同样执行分离 |
| kernel/vm.c：kalloc / kfree | 维护物理页引用、位图和物理页计数 |
| kernel/vm.c：pmc_read | 读取实际复制总数、COW 共享物理页数 |
| user/init.c | 启动 Shell，并持续回收被收养的孤儿进程 |

SYS_pmc=26 严格采用本轮接口；之前私有的 pagecount 从 26 迁至 27，定义放在本地 kernel/lab5.h，旧 vmstress 和 smpstress 同步迁移。课程 kernel/syscall.h、user/user.h、user/usys.pl 与 gifts 对应原件一致。

## 2. 引用计数方案及空间

必须采用全局数组方案。references[(pa-KERNBASE)/PGSIZE] 记录每页的有效所有权引用；cow_origin 同索引标记该页是否来自可写用户页的 COW 共享。分页数据结构、内核栈、trapframe 等 kalloc 页也从引用 1 开始，但不会进入用户 COW。

128MiB 物理范围对应 32768 个 4KiB 页，32 位 references 数组为 128KiB，字节 cow_origin 数组为 32KiB，合计 160KiB。它们属于静态 BSS，kernel_end 包含这部分空间，因此不会误当空闲页分配。每个进程自己的页表及独立内核资源仍按原机制分配。

数组和现有分配位图共用 kmem_lock，优点是分配、引用降零及位图回收能原子衔接，锁顺序简单；缺点是并发分配/释放时会争用一把锁。临界区只处理索引和计数，不在锁内做 4KiB 清零/复制、页表分配或打印。

其余两种方案的权衡：PTE RSW 只有两位，无法自然保存任意共享次数，且不同进程的 PTE 需要协调和溢出处理；扫描页表方案避免完整引用数组，但每次判断需遍历页表并稳定进程/页表生命周期，锁顺序与扫描成本更复杂。本实现仅用 RSW bit 8 标记 COW，不在 PTE 中编码引用数。

## 3. 引用计数生命周期时机表

| 事件 | 操作 | 持锁与失败处理 |
|---|---|---|
| kalloc | 位图占用、references=1、cow_origin=0 | kmem_lock 内原子更新，解锁后清零，返回前不发布页 |
| fork 共享 | 旧页引用 +1；原 W 页清 W、置 COW | kmem_lock 保护引用和共享统计；先取得有效引用再建立子 PTE |
| 子映射建立失败 | 撤销刚增加的引用 | kfree 只减一次；freeproc 另行清理此前已成功建立的映射 |
| COW 引用数为 1 | 原 PTE 恢复 W、清 COW | 在 kmem_lock 内判定并更新，刷新 TLB；无实际复制 |
| COW 引用数大于 1 | 分配新页为 1；复制；换 PTE；旧页 -1 | 复制期间始终持有旧引用；不持 kmem_lock 调 kalloc/kfree |
| exec 成功 | 新页表提交后，销毁旧用户映射并逐页 -1 | 新映像全部构造成功前不修改旧进程的映像字段 |
| exec 失败 | 释放 staging 页和新页表已取得的页 | 保留旧页表、epc、sp 和程序数据；返回 -1 |
| sbrk 缩减 | 清叶子 PTE，旧页 -1；空页表逐级释放 | 复用 uvmdealloc 和 kfree，随后刷新 TLB |
| exit / kill | 先关闭管道、重设父子关系、成为 ZOMBIE | 不直接释放仍在使用的内核栈和 trapframe |
| wait / freeproc | 销毁叶子映射逐页 -1，释放独立内核资源 | 父进程取得子进程锁，保证子进程已切离自身内核栈 |
| 引用从 1 降到 0 | 清 COW 来源标记，归还位图，allocated_count-- | 与引用递减在同一个 kmem_lock 临界区完成 |

失败 fork 可能让父页继续保留只读 COW 标记，但子映射和引用已完全回滚；以后写入时若引用只剩 1，直接恢复 W。因此不要求失败 fork 立即把父权限逐页改回去。

少加一次引用：一个进程退出就可能把另一进程仍使用的页归还，后续分配重用导致串数据或悬空映射。少减一次引用：页永远达不到 0，重复 fork/exec 后泄漏。多减一次：页过早释放，最终可能重复释放。kfree 对已空闲页/零引用重复释放输出 panic，压力日志检查此类错误。

## 4. 缺页决策树与特殊页面

```text
用户陷入
├─ scause=8：系统调用，epc += 4
├─ 时钟/设备中断：沿用 Lab4 调度与设备分支
├─ scause=15：写页故障
│  ├─ va 越界、PTE 不存在、非 V/U 或没有 COW：exit(-1)
│  └─ 合法 COW 页
│     ├─ 已 killed：不启动新的复制，返回失败并终止
│     ├─ refs=1：恢复 W，清 COW，无复制
│     └─ refs>1：分配、复制、发布新 PTE、旧引用 -1
│        └─ 分配失败：保持旧映射，终止故障进程
└─ 其他用户异常：exit(-1)
```

成功处理 COW 后重试原写指令，不能将 epc 加 4。更新 PTE 后执行 sfence_vma；fork 清父 W 后也刷新父 TLB。trampoline 在切换 satp 前后刷新 TLB，同一进程不会同时被两核派发，所以迁移后不会继续使用另一核保留的旧可写用户翻译。每次返回用户态执行本核 fence.i，覆盖 exec 后迁移时的指令缓存同步。

trampoline 是内核只读执行页；trapframe 和内核栈为各进程独立资源，不能进入 COW。真正只读的 text/rodata 可在 fork 时共享引用，但绝不打 COW 标记，写代码页必须被杀，不能升级为可写。

当前仍使用固定一页用户栈及两页 guard。不把 guard 缺页误判为栈增长；野指针、guard、真正只读页都走非法访问分支。Lab3 guard 回归继续成立。

## 5. 内核写用户内存

usercheck(write=1) 接受 W 或合法 COW 权限，不预先复制。usercopy(to_user=1) 在每个目标页写入前调用 cow_resolve，然后重新翻译物理地址。读取用户内存不触发复制。

因此 console read、piperead、pipealloc 返回 fd、wait 写 status、schedstat 写统计等路径不会因为 fork 清 W 而错误拒绝，也不会绕过页表直接污染共享页。跨页时逐页处理；后续页失败允许返回错误，但不会泄漏已完成页或破坏旧页引用。当前接口不承诺多页 copyout 的内容原子性。

## 6. 双核原子性与锁顺序

进程仍由原有 p->lock 保证唯一派发。用户页表只由当前进程的内核执行流操作；尚未发布的子页表由 fork 构造者独占，退出后的页表由 wait 持子进程锁回收。其他进程只共享物理页，不共享用户页表对象。

两核同时复制旧页时，两者在各自 PTE 发布新页以前都保留旧引用。因此即使另一核 exec、exit 或先完成分离，旧页也不能被释放并重用。若观察到 refs=1，就不存在另一有效共享映射可以并发复制/写入该页；恢复 W 无需分配。

锁顺序继承 Lab4 的条件锁/等待锁 → 进程锁；某些 copyout 会在持有管道锁或子进程锁时取得 kmem_lock。kmem_lock 内不取得任何进程锁、管道锁、输出锁，不调用可能再次取 kmem_lock 的函数，也不 sleep/swtch。这样不会形成反向锁依赖。4KiB 复制在 kmem_lock 外完成，跨核可并行复制不同页面。

## 7. PMC、预算及 kill 语义

pmc(0) 是 u64 全系统累计“用户物理页完整克隆”次数。只在 cow_resolve 真正执行 4KiB 复制后增加；即使尝试取消了已执行的复制，也不能隐去这次实际工作。ELF 从内嵌文件装入新页、trapframe 字节拷贝和独占页恢复权限不算 COW 物理页克隆。

pmc(1) 按物理页去重：cow_origin 为真且引用数至少为 2 时计为 1。1→2 增加，2→1 减少；普通只读代码共享不计入 COW 共享。fork 在建立 PTE 前短暂预留的引用也属于受保护的有效所有权，失败时会撤销。不能把共享 PTE 数、引用总数或所有已分配页数冒充此值。非法 which 返回 u64 的 -1。

预算选择“每进程当前程序映像”口径：allocproc / fork 新子进程从 0 开始，成功 exec 重置；一次实际 COW 克隆记一次。前 143 次属于预算，尝试继续复制时仍正常安全分离，第 144 次发出一次 COW-BUDGET 报告，此后限频不重复报告；采用课程允许的“异常报告后继续运行”行为。PMC 全局计数不重置、不封顶。预算语义与 pmc 统计分开，避免旧的 200/300 次 fork 压力被全局 143 次阈值误杀。

kill 参数为 1：cow_resolve 开始前已收到 kill，则不启动新复制；开始后另一核发来 kill，允许当前页复制、PTE 发布及旧引用递减完成，再在安全用户返回边界终结。不从发送 kill 的核释放目标进程的页表或内核栈。复制后的多页操作若还需分离新页，会重新检查 killed，避免继续启动新复制。

## 8. exec 参数与映像提交

sys_exec 使用 userstring（argstr 的本地实现）安全读取字符串；逐个读取用户 argv 指针并检查 NULL 终止，最多 32 个参数，总暂存字符串最多一页。所有内容先拷入内核 staging 页，避免切换页表后继续解引用旧用户地址。非法名称、一级/二级指针、缺少终止符、超量参数或空间不足均返回 -1。

程序仍来自 _uprog_table 内核内嵌表，没有引入磁盘文件系统。构建改用本地 support/user-lab5.ld，把 ELF 的 text、rodata、data/bss 按页分段，保留课程 user/user.ld 原件。load 检查 ELF 及段范围，用 RX、R、RW 各自权限建立映射，BSS 由 kalloc 清零，真实入口来自 ELF。入口 start 包装器调用 main，main 返回后调用 exit。

先把参数字符串从栈顶向下放置，各步对齐到 16 字节，再放入以 NULL 结尾的新用户指针数组。最终 sp 和 a1 都指向 argv，a0=argc。load 成功返回 argc，syscall 最终写 a0 时便不会把 argc 覆盖为 0。只有全部映像、参数栈成功后才替换旧页表和寄存器字段，并释放旧页表；失败仅销毁 replacement 和 staging 页。

Shell 保持课程最小版本，没有额外增加 Shell 语法；多参数 ABI 由 exectest/argcheck 直接验证，不依赖 Shell 参数解析。

## 9. 父进程先退出与孤儿回收

旧实现把 Shell 本身当 init。Shell 回到输入等待后不会持续 wait 孤儿，因此“父先退出”的 COW 页可能长期被僵尸占有。新增独立用户 init 启动 Shell，并循环 wait 所有被收养子进程；原来的 proc_exit 重设父子关系和唤醒逻辑继续使用。子进程先退出、父先退出、kill 后回收都最终经过同一 freeproc 引用递减路径。

## 10. 测试、实测读数与复现

在 WSL 的项目目录运行：

```sh
python3 support/check_lab5.py --cpus 1
python3 support/check_lab5.py --cpus 2 --repeat 3
```

驱动会先构建、检查 Banner 格式，再逐项执行 Lab5、Lab3、Lab2 及对应核数的 Lab4 脚本。除了 EXPECT，还扫描真实 UART 日志中的 FAIL/WARN/panic/kernel trap，避免只看到最后一条 PASS。预算报告是预期诊断。原始日志保存在 support/lab5-logs（*.log 不跟踪），成功后自动合并至可归档的 support/lab5-validation.txt；避免 WSL 临时目录跨调用丢失证据。

| 程序 | 验证内容 |
|---|---|
| zerocopy（官方原文） | fork 后立即 exec，实际复制增量 ≤2 |
| cowpair（官方原文） | 共享存在、父子写后隔离、当前构建的实际复制增量为 2 |
| zeroraw | 用寄存器和直接 ecall 避免 fork 后旧栈写入，真实复制增量为 0 |
| exectest / argcheck | argc/argv、空参数列表、跨页字符串、NULL、对齐、非法指针、超长/过量参数和失败无泄漏 |
| cowstress | 六进程高频写16个共享页、数据读回、共享计数回基线、内核 copyout、嵌套 fork、只读保护、24次kill、预算超限、父先退出 |
| cowoom | 分配耗尽后逐步暴露0–15页空间，覆盖部分fork/exec失败清理；共享页写缺页OOM时子终止、父数据保留 |
| 既有回归 | badecall、bufstorm、badaccess、memcap、vmstress、bench、lottery_test、stride_test（补充）、smpstress |

官方 zerocopy 在 C 代码中构造栈上 argv，正常会引发一次旧栈 COW，实测增量为 1；因此单独提供 zeroraw 展示真正零次，而没有修改官方源码或计数口径。

cowpair 的 exact 2 是当前程序布局及调度下的实测结果，不是任意 COW 实现的数学不变式：栈写入也进入全局 PMC，引用数变成1后可不复制。补充测试通过同步、父子读回和回收后页数/共享数基线检查验证隔离与生命周期，不依赖凑次数。官方两项源码及构建副本均与增量 ZIP 对应文件一致。

最终验证范围：单核一轮完整回归，双核三轮独立启动完整回归；每轮双核 Lab4 脚本包含五次压力测试，每次6个工作进程、合计300次 fork。实际 UART 结果合并保存在 support/lab5-validation.txt。有限测试不能替代并发形式化证明，教师现场问答仍需本人掌握上述机制。

## 11. 现场演示与交付

在 sh> 依次执行 zerocopy、zeroraw、cowpair、exectest、cowstress、cowoom、bench、lottery_test。双核用既有 UART 驱动 --cpus 2 启动；手工启动也可使用 qemu-system-riscv64 -machine virt -bios none -kernel kernel/kernel -nographic -smp 2。

交付前提交代码和设计笔记，打 lab5-submit，再从该标签执行 git archive 导出 提交-lab5-2024302111375.zip。生成归档不纳入 Git 跟踪，构建产物及临时日志保持忽略。
