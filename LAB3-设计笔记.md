# Lab3 内存管理设计笔记

## 地址空间

每个进程持有独立的 SV39 根页表。低地址 `0` 不映射；用户程序从 `0x1000` 链接并加载，因而写零地址触发用户页故障。堆从程序镜像末尾向上增长。用户栈位于 `USER_STACK_BASE=0x7fffc000` 起的一页，栈底下方 `0x7fffa000–0x7fffc000` 留出 `LAB3_GUARD_PAGES=2` 个未映射页。用户页使用 U 权限；trapframe 与 trampoline 均不带 U 权限。设备与内核直映只在独立的内核页表中出现。

| 区域 | R | W | X | U |
| --- | --- | --- | --- | --- |
| 用户程序镜像 | 1 | 1 | 1 | 1 |
| 堆和用户栈 | 1 | 1 | 0 | 1 |
| 栈保护页和零页 | 无映射 | 无映射 | 无映射 | 无映射 |
| trapframe | 1 | 1 | 0 | 0 |
| trampoline | 1 | 0 | 1 | 0 |
| 设备 | 1 | 1 | 0 | 0 |

现阶段嵌入式平坦镜像不携带分段权限元数据，故程序镜像暂用 RWX。内核物理内存采用 2 MiB 大页直映，设备采用 4 KiB 页映射。`satp` 在返回用户态前切至当前进程页表；trapframe 中的 `kernel_satp` 指向内核页表，进入内核后由 trampoline 恢复。课程链接脚本未把代码与数据分开对齐到页边界，当前内核直映仍需 RWX；用户进程无法访问该映射。

## 分配与回滚

三级位图管理所有可分配物理页。`kalloc` 始终选取最高的空闲页编号，释放后也不改变顺序，满足个人参数 `LAB3_ALLOC_ORDER=1`。`allocated_count` 统计当前分配页，达到总可用页减 `LAB3_MEMCAP_DELTA=15` 时 `kalloc` 返回空指针。`SYS_pagecount=23` 是本实验自测用观测接口。

| 成功路径 | 失败回滚 | 返回时保证 |
| --- | --- | --- |
| `uvmalloc` 分配页并安装 PTE | 先清除本次已装 PTE，再释放其物理页，并清除空的中间页表 | 旧映射与配额计数保持有效 |
| `fork` 分配子页表、复制用户页 | 递归释放子页表及已复制的用户页 | 父进程地址空间不变 |
| `exec` 在新页表中加载镜像 | 释放未提交的新页表；成功后切换 `satp` 再释放旧页表 | 失败仍可执行旧镜像 |
| `sbrk` 扩展堆 | `uvmalloc` 回滚本次增量，进程大小不变 | 已有堆页仍可访问 |

系统调用通过 `usercopy` 按页验证 V、U 及 R/W 权限，再拷贝用户数据；`write`、`read`、`exec`、`wait` 不直接解引用用户地址。用户页故障通过 `usertrap` 使进程以非零状态退出，父进程仍可回收。

## 观测与回归

首个 Shell 页表装配后自动执行 `dump_pagetable`。输出使用 `DUMP-PAGETABLE begin/end`、`L2`、`L1`、`LEAF` 和 `va/pa/perm` 字段。Shell 等待输入时可发送 `Ctrl-P` 重放当前进程的页表。QEMU 已验证 `hi`、`badecall`、`bufstorm`、`badaccess`、`memcap`；`vmstress` 验证跨页指针、零指针、保护页、收缩堆、大额分配失败后页数不变，以及 200 次 fork/exec 后页数不变。`memcap` 耗尽物理配额后再运行 `vmstress` 仍通过。

验收复现：先运行 `make -j4`，再运行 `python3 support/inject_uart.py --tree . --script support/tests/lab3-uart.script --log lab3-uart.log`。Lab2 输入回归使用 `support/tests/lab2-uart-regression.script`。验收后生成的串口日志无需纳入提交。
