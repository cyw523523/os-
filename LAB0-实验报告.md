# Lab0 热身实验报告

学号：2024302111375  
姓名：蔡以威

## 1. 实验目标与阅读路线

本实验追踪用户输入 `echo hi` 后，从 Shell 读取命令到屏幕显示 `hi` 的完整生命周期，重点观察：

- 用户态和内核态之间的切换；
- 系统调用从 `ecall` 到内核分发的路径；
- 进程的 `fork`、`exec`、`wait`、`exit` 状态流转；
- UART 输入中断、睡眠和唤醒；
- 页表、文件描述符和 inode 的引用关系；
- 时钟中断进入内核并通过调度器返回用户态的过程。

推荐阅读顺序：

1. `user/sh.c`：Shell 主循环和命令执行；
2. `user/usys.pl`、`kernel/syscall.c`：系统调用入口和分发；
3. `kernel/trap.c`、`kernel/trampoline.S`：陷入、异常和中断；
4. `kernel/exec.c`、`kernel/vm.c`：用户地址空间和程序装载；
5. `kernel/file.c`、`kernel/console.c`、`kernel/uart.c`：文件描述符到 UART 的输出链路；
6. xv6 book 的 “Operating system interfaces” 和 “Traps and system calls” 章节。

阅读每个核心函数时，始终记录三个问题：

1. 谁调用它？
2. 它是否可能睡眠？如果睡眠，谁负责唤醒它？
3. 出错时如何回滚？

## 2. 材料一：`echo hi` 全系统控制流图

### 2.1 总体流程

```text
用户在 UART 输入 "echo hi\n"
        |
        v
UART 接收字符并产生中断
        |
        v
内核处理 UART 中断，将字符放入 console 输入缓冲区
        |
        +--> 若 Shell 正在 consoleread 中睡眠，则唤醒 Shell
        |
        v
Shell 的 read(0, buf, ...)
        |
        v
Shell 解析命令并 fork
        |
        +------------------------------+
        |                              |
        v                              v
父进程 Shell                         子进程
等待子进程结束                       runcmd -> exec("echo", argv)
wait -> sleep                         |
        |                              v
        |                        新的 echo 用户地址空间
        |                              |
        |                              v
        |                        echo main
        |                              |
        |                              v
        |                        write(1, "hi", 2)
        |                              |
        |                              v
        |                        usertrap -> syscall -> sys_write
        |                              |
        |                              v
        |                        filewrite -> consolewrite
        |                              |
        |                              v
        |                        UART 发送寄存器
        |                              |
        |                              v
        |                        屏幕显示 "hi"
        |                              |
        |                              v
        |                            exit
        |                              |
        +<-----------------------------+
        |
        v
父进程 wait 被唤醒并回收子进程
        |
        v
Shell 回到主循环，再次等待下一条命令
```

### 2.2 Shell 读入命令

Shell 启动后在 `user/sh.c` 的 `main` 中循环执行：

```text
打印提示符
-> read(0, buf, sizeof(buf))
-> 解析一行命令
-> fork
-> 子进程执行命令
-> 父进程 wait
```

`read` 的完整调用链可以概括为：

```text
Shell 用户代码
  -> read 系统调用封装
  -> ecall
  -> trampoline.S 的用户态陷入入口
  -> usertrap
  -> syscall
  -> sys_read
  -> fileread
  -> consoleread
  -> sleep（输入缓冲区暂无完整输入）
  -> scheduler
```

当用户在 UART 输入字符时：

```text
UART 硬件接收字符
  -> UART 中断
  -> devintr
  -> uartintr
  -> consoleintr
  -> 将字符写入 console 输入缓冲区
  -> wakeup(&input.r)
  -> Shell 从 sleep 返回
  -> consoleread 将一行数据复制到用户缓冲区
  -> sys_read 返回
  -> usertrapret
  -> sret 回到 Shell 用户代码
```

没有输入或输入尚未形成一整行时，Shell 不能继续解析命令，因此在 `consoleread` 中睡眠。睡眠会把进程状态置为 `SLEEPING`，随后调用调度器让出 CPU。UART 中断处理程序写入输入缓冲区并调用 `wakeup`，使等待该通道的 Shell 变为可运行状态。

### 2.3 `fork`、`exec` 和 `wait`

Shell 解析出 `echo hi` 后调用 `fork`。成功后产生父子两个执行流：

```text
父进程 Shell：
    fork 返回子进程 PID
    -> wait(0)
    -> 等待子进程退出

子进程：
    fork 返回 0
    -> runcmd
    -> case EXEC
    -> exec("echo", argv)
```

`fork` 复制进程运行所需的状态，使父子进程从系统调用返回后的逻辑位置继续执行，但返回值不同：

- 父进程收到新建子进程的 PID；
- 子进程收到 0；
- 如果创建失败，父进程收到负值并执行错误处理。

子进程执行 `exec` 后：

1. 根据文件名找到 `echo` 程序；
2. 创建新的用户页表；
3. 将程序代码和数据装入新的地址空间；
4. 建立用户栈、guard page、trapframe 和 trampoline 映射；
5. 设置新的用户程序入口地址和栈指针；
6. 提交新的地址空间；
7. 释放旧地址空间；
8. 返回用户态后，CPU 从新的 `echo` 程序入口开始执行。

因此，`exec` 成功后，进程的 PID 和内核进程对象仍然是原来的子进程，但原来 Shell 子进程的用户地址空间和代码已经被替换，不能继续执行原来的 `runcmd` 路径。

父进程调用 `wait` 后，如果子进程尚未退出，父进程睡眠。子进程调用 `exit` 后，内核关闭其文件描述符、释放部分资源并将其状态变为 `ZOMBIE`，然后唤醒等待它的父进程。父进程从 `wait` 返回并回收子进程的剩余内核资源。

### 2.4 `write` 到 UART 的路径

`echo` 的用户代码执行：

```text
write(1, "hi", 2)
```

完整路径：

```text
echo 用户态 write
  -> ecall
  -> trampoline.S 保存用户寄存器
  -> usertrap
  -> syscall
  -> sys_write
  -> argfd / argaddr / argint 检查参数
  -> filewrite
  -> consolewrite
  -> consputc
  -> uartputc
  -> 写 UART 发送寄存器
  -> UART 输出字符 h、i
  -> sys_write 返回
  -> usertrapret
  -> sret 回到 echo 用户态
```

`1` 是标准输出文件描述符。该描述符最终指向一个内核 `file` 结构，而该结构通过 inode 或设备信息指向 console 设备。console 驱动再调用 UART 驱动，把字符写入硬件发送寄存器。

## 3. 栈、特权级和锁标注

| 阶段                         | 运行栈               | 特权级      | 锁情况                        |
| -------------------------- | ----------------- | -------- | -------------------------- |
| Shell 执行 `read` 前          | Shell 用户栈         | U 态      | 不持有内核自旋锁                   |
| `ecall` 后的陷入入口             | 当前进程内核栈           | S 态      | 按陷入处理路径取得必要锁               |
| `sys_read` / `consoleread` | Shell 内核栈         | S 态      | console 输入缓冲区锁；睡眠前按规则释放相关锁 |
| 进入 `scheduler`             | 调度器专用栈            | S 态      | 调度切换涉及进程锁                  |
| UART 中断处理                  | 被中断进程的内核栈         | S 态      | UART 锁、console 锁等由驱动按顺序管理  |
| Shell 从睡眠返回                | Shell 内核栈         | S 态      | 返回前重新取得所需锁                 |
| Shell 执行 `fork`            | Shell 内核栈         | S 态      | 进程表和子进程状态操作涉及进程锁           |
| 子进程执行 `exec`               | 子进程内核栈            | S 态      | 地址空间和文件操作按对应模块管理           |
| echo 执行用户代码                | echo 用户栈          | U 态      | 不持有内核自旋锁                   |
| echo 执行 `write`            | echo 内核栈          | S 态      | 文件和 console 路径可能取得设备/文件锁   |
| 调度器选择下一个进程                 | 调度器专用栈            | S 态      | 进程状态切换期间涉及进程锁              |
| `sret` 返回用户态               | 先在当前进程内核栈，返回后为用户栈 | S 态到 U 态 | 返回用户态前应释放不应跨越调度的锁          |

M 态主要用于系统启动早期。Lab0 的 `echo hi` 正常执行链路主要在 U 态和 S 态之间切换；UART 中断由内核在 S 态处理。

## 4. 材料二：核心数据结构全景快照

### 4.1 快照时刻

本快照选择：

```text
echo 的 exec 已经成功完成，
但 echo 的第一条用户指令尚未执行。
```

此时 Shell 已经 fork 出子进程，子进程已经用 `exec` 替换为 echo 程序。Shell 父进程通常正在 `wait`，echo 进程处于可运行或即将被调度运行的状态。

### 4.2 进程表快照

下表是结构关系快照。具体 PID、地址和 `sz` 应以实际参考树运行时输出为准。

| 进程       | 典型状态                    | parent     | pagetable   | sz         | 文件描述符                     |
| -------- | ----------------------- | ---------- | ----------- | ---------- | ------------------------- |
| init     | `RUNNABLE` 或 `SLEEPING` | 无或内核规定的父进程 | init 页表     | init 镜像大小  | 0、1、2 通常指向 console        |
| shell    | `SLEEPING` 或 `WAITING`  | init       | Shell 页表    | Shell 镜像大小 | 0 stdin、1 stdout、2 stderr |
| echo 子进程 | `RUNNABLE`              | shell      | 新建的 echo 页表 | echo 镜像大小  | 继承 Shell 的 0、1、2          |

需要区分两个时刻：

- `exec` 完成、尚未运行 echo：echo 通常等待调度，状态可能是 `RUNNABLE`；
- echo 已经运行并执行 `write`：它可能在系统调用中处于 `RUNNING`，Shell 则可能在 `wait` 中睡眠。

`fork` 后，子进程继承父进程的打开文件描述符引用。父子进程的 `ofile[1]` 因而可以共同引用标准输出对应的 `file` 结构，引用计数保证其中一个进程退出时不会错误地释放另一个进程仍在使用的对象。

### 4.3 echo 页表快照

echo 的用户虚拟地址空间可抽象表示为：

```text
高地址
0xFFFFFFFFFFFFFFFF
        |
        | trampoline：内核与用户态切换代码
        | 只读、可执行，通常不允许用户写
        |
        | trapframe：保存用户寄存器的页面
        | 用户不可访问，由内核访问
        |
        | -------------------------------
        | 用户栈
        | guard page：阻止栈越界
        | -------------------------------
        | echo 代码段：R-X、U
        | echo 数据段：R-W、U
        | 堆：R-W、U，位于 sz 以内
        |
0x0000000000000000
低地址
```

典型权限含义：

| 区域         | 典型权限              | 用户可访问 |
| ---------- | ----------------- | ----- |
| trampoline | `V,R,X`，通常无 `U`   | 否     |
| trapframe  | `V,R,W`，无 `U`     | 否     |
| 代码段        | `V,R,X,U`         | 是     |
| 数据段        | `V,R,W,U`         | 是     |
| 用户栈        | `V,R,W,U`         | 是     |
| guard page | 不建立有效用户映射，或不设 `U` | 否     |
| 堆          | `V,R,W,U`         | 是     |

trampoline 位于用户地址空间顶部附近，使用户态陷入和返回时可以使用固定虚拟地址；它由多个进程共享同一份代码页，但应保持只读和不可由用户态写入。trapframe 保存每个进程自己的寄存器现场，因此每个进程需要独立的 trapframe 页面。

实际绘图时，应从课程参考树的页表宏和 `proc_pagetable`、`uvmfirst`、`exec` 等函数中抄录准确的虚拟地址边界；不要直接把本表中的示意地址当成最终数值。

### 4.4 文件表快照

标准输出的引用链为：

```text
echo 进程
  -> proc.ofile[1]
  -> struct file
  -> inode / device information
  -> console device
  -> consolewrite
  -> consputc
  -> uartputc
  -> UART transmit register
  -> 屏幕
```

Shell fork 出 echo 时，子进程继承文件描述符数组。`echo` 的 `write(1, ...)` 通过 `ofile[1]` 找到标准输出对象，并由文件层分派到 console 设备。echo 退出时关闭其文件描述符，文件引用计数归零后才释放底层 `file` 结构；这样可以避免父子进程共享标准输出时提前释放资源。

## 5. 材料三：一次时钟中断的微观旅程

### 5.1 中断入口

选择一个正在运行用户代码的进程。时钟中断到达时：

```text
scause = 0x8000000000000005
```

最高位为 1，表示这是中断；低位 5 表示 Supervisor timer interrupt。

流程如下：

```text
用户进程在 U 态执行
        |
        v
时钟中断到来
        |
        v
硬件切换到 S 态，并跳到 stvec
        |
        v
trampoline.S 的 uservec
        |
        v
切换到内核页表，保存用户寄存器到 trapframe
        |
        v
usertrap
        |
        v
识别 scause = 0x8000000000000005
        |
        v
devintr / 时钟处理
        |
        +--> 更新时间、唤醒到期进程等
        |
        v
决定当前进程是否 yield
        |
        v
切换到 scheduler 专用栈
        |
        v
scheduler 选择另一个 RUNNABLE 进程
        |
        v
swtch 恢复被选进程的内核上下文
        |
        v
该进程经过 usertrapret 准备返回用户态
        |
        v
trampoline.S 恢复寄存器
        |
        v
sret
        |
        v
回到用户态继续执行
```

### 5.2 现场保存与恢复

进入 `usertrap` 前，硬件和 trampoline 共同完成特权级切换及寄存器现场保存。内核需要保存用户寄存器，原因是中断处理和调度过程会使用内核寄存器，返回用户态时必须恢复用户程序原来的执行现场。

`usertrap` 主要完成：

1. 确认陷入来自用户态；
2. 设置内核陷阱向量；
3. 保存用户程序计数器；
4. 读取 `scause`，判断是系统调用、设备中断还是异常；
5. 对时钟中断调用设备中断处理逻辑；
6. 对需要抢占的情况调用 `yield`；
7. 通过 `usertrapret` 设置下一次用户态陷入入口和返回状态；
8. 进入 trampoline，恢复寄存器并执行 `sret`。

如果时钟中断导致当前进程让步，当前进程的状态会被设置为 `RUNNABLE`，然后通过 `sched` 将执行权交给调度器。调度器运行在每个 CPU 的调度器专用栈上，遍历进程表选择其他可运行进程。被抢占的进程不会丢失用户态执行位置，因为其用户寄存器和 `sepc` 已保存到自己的 trapframe。

## 6. 关键问题回答

### 6.1 fork 返回后父子进程从哪里继续？

父进程和子进程都从 `fork` 系统调用返回后的下一条用户指令继续执行。区别在于系统调用返回值不同：父进程得到子进程 PID，子进程得到 0。内核通过分别设置两个进程的返回寄存器实现这一点。

### 6.2 exec 后地址空间发生了什么？

`exec` 读取新的 ELF 程序，创建新的页表和新的用户内存布局，装入代码段、数据段和用户栈，然后设置新的入口地址和栈指针。提交成功后释放旧的用户地址空间，但进程自身的 PID、内核进程对象和通常的文件描述符仍然保留。

### 6.3 trampoline 为什么可以共享？

trampoline 是内核提供的固定陷入/返回代码，不保存某一个进程独有的长期状态。多个进程可以映射同一个物理代码页；为了防止代码被修改，该页应设置为只读，并且不允许普通用户态写入。

### 6.4 Shell 在 read 返回前是什么状态？谁唤醒它？

当输入缓冲区没有完整一行时，Shell 在 `consoleread` 中睡眠，状态为 `SLEEPING`。UART 中断到来后，内核把字符写入 console 输入缓冲区，并调用 `wakeup` 唤醒在对应通道睡眠的 Shell。

### 6.5 write 如何变成 UART 寄存器写入？

用户 `write` 通过 `ecall` 进入内核，经 `usertrap` 和 `syscall` 分发到 `sys_write`，再经过文件描述符查找、`filewrite`、`consolewrite`、`consputc` 和 `uartputc`，最后由 UART 驱动把字符写入发送寄存器。UART 硬件随后将字符发送到模拟串口，QEMU 的 `-nographic` 将其显示在终端。

### 6.6 如果不创建第一个用户进程会怎样？

内核仍可能完成早期机器和内存初始化，但没有用户进程可运行，调度器找不到可运行的用户进程，系统通常表现为没有用户态输出、停留在调度或空转路径。具体停留位置需要结合课程骨架的启动代码和 QEMU 日志确认。

### 6.7 子进程退出但父进程尚未 wait 时是什么状态？

子进程进入 `ZOMBIE` 状态，保留必要的退出状态和 PID，等待父进程调用 `wait` 回收。父进程调用 `wait` 后，内核释放子进程剩余的进程表资源。

### 6.8 exit 如何避免文件资源泄漏？

退出时内核遍历进程的打开文件描述符并逐个关闭，减少对应 `file` 结构的引用计数。父子进程可能共享同一个文件对象，所以只有引用计数归零时才能真正释放底层资源。

## 7. 自主思考批注

### 批注一：sleep 和 wakeup 的通道必须一致

如果 `consoleread` 睡眠时使用的通道和 UART/console 中断处理程序调用 `wakeup` 使用的通道不一致，Shell 会一直睡眠，即使输入字符已经到达。因此睡眠通道是阻塞方和唤醒方之间的重要协议。

### 批注二：系统调用返回地址必须正确推进

执行 `ecall` 后，如果内核返回时没有让 `sepc` 增加一条指令的长度，进程会再次执行同一条 `ecall`，表现为系统调用不断重复进入，甚至形成无限陷入。

### 批注三：页表权限错误会改变故障类型

如果代码页被错误地设置为不可执行，或者用户页没有 `U` 权限，进程返回用户态后可能立即出现取指访问错或权限异常。使用 QEMU `-d int` 观察 `cause`、`epc` 和 `tval`，再用 `addr2line` 还原源码位置，是定位此类问题的直接方法。

### 批注四：wait 必须处理并发状态变化

父进程检查子进程状态和进入睡眠之间必须遵守锁和睡眠协议，否则子进程可能在父进程真正睡眠前退出，导致唤醒信号丢失，父进程永久等待。

### 批注五：fork 的文件引用不能简单复制指针

父子进程可以共享同一底层文件对象，但必须增加引用计数。如果只复制指针而不增加引用计数，任一进程退出关闭文件后，都可能让另一个进程继续使用已释放的对象。

## 8. 调试与故障定位速查

### 8.1 退出 QEMU

```text
Ctrl-a，松开，再按 x
```

### 8.2 查看异常和中断

```bash
qemu-system-riscv64 -machine virt -bios none \
  -kernel kernel/kernel -nographic \
  -d int -D int.log &
QPID=$!
sleep 5
kill $QPID

grep 'async:0' int.log | head
```

常见原因：

| cause                | 含义        | 常见问题             |
| -------------------- | --------- | ---------------- |
| 1                    | 取指访问错     | 入口地址、PMP 或跳转地址错误 |
| 2                    | 非法指令      | 跳入数据区或代码损坏       |
| 5 / 7                | 读/写访问错    | MMIO 地址或设备基址错误   |
| 8                    | U 态 ecall | 正常系统调用入口         |
| 12 / 13 / 15         | 缺页        | 页表权限、映射或 COW 问题  |
| `0x8000000000000005` | S 态时钟中断   | 正常时钟抢占           |

### 8.3 查看最后执行的指令

```bash
qemu-system-riscv64 -machine virt -bios none \
  -kernel kernel/kernel -nographic \
  -d in_asm -D trace.log &
QPID=$!
sleep 3
kill $QPID

tail -40 trace.log
riscv64-unknown-elf-objdump -d kernel/kernel > kernel.asm
riscv64-unknown-elf-addr2line -e kernel/kernel 0x80000123
```

### 8.4 QEMU Monitor

```text
Ctrl-a c
(qemu) info registers
(qemu) info cpus
Ctrl-a c
```
