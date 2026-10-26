# Lab4 设计与复现

学号 2024302111375，`LAB4_POLICY_ID=5`，彩票调度；单核必做与双核加分路径均已实现。课程包中的 `gifts/` 与 `support/tests/` 原件留存；构建使用的测试副本在 `user/`。

## 控制流与状态

```text
用户栈(U) → trampoline/trapframe(S) → 当前进程内核栈(S)
  → acquire(p->lock) → RUNNABLE/SLEEPING/ZOMBIE → swtch
  → 调度器栈(S) → 按票数抽签 → RUNNING → swtch
  → 目标进程内核栈(S) → release(p->lock)
  → trampoline/sret → 目标进程用户栈(U)
```

```text
UNUSED → USED → RUNNABLE → RUNNING → RUNNABLE   (时钟片用尽/yield)
                          ↘ SLEEPING → RUNNABLE (wakeup/kill)
                          ↘ ZOMBIE → UNUSED    (exit/wait)
```

每个进程有一页独立内核栈、trapframe、页表和 14 个被调用者保存寄存器的 `context`。初次调度从 `forkret` 恢复；后续从 `sched` 内 `swtch` 的下一条指令恢复。通用寄存器中的调用者保存部分由 C 调用约定或 trapframe 保存；返回地址 `ra` 充当恢复 PC。

## 策略

每次从 RUNNABLE 进程的票数总和中均匀抽一个整数，按进程表顺序做前缀抽签。默认票数为 `7 + sid % 13`；执行 `hi`、`spin`、`sh` 时分别改为 1、2、4。用户 `setickets` 可覆盖当前票数，供课程测试在同一种进程名下检验比例。时钟中断按 `LAB2_TICK=2` 抢占；调度重新分发时清空连续片长，累计 CPU tick 不清零。

抽签使用 xorshift64 伪随机状态。对持续就绪的进程，单次中选概率为其票数除以总票数，连续未中概率按窗口指数下降；有限观察窗口仍有统计波动，所以测试使用容差。低票进程有正概率，并未给出严格的最坏等待时间界。

## 核心不变式

1. `sched()` 前恰好持有当前进程的锁、关闭中断，进程状态已不再是 RUNNING。
2. 调度器在持有目标进程锁时设为 RUNNING 并切入；进程恢复后释放同一把锁。
3. `sleep(chan, lock)` 先持进程锁再释放条件锁，设置 SLEEPING 后切换；唤醒者持进程锁修改状态，避免检查条件与睡眠之间丢失唤醒。
4. ZOMBIE 的内核栈、trapframe 和页表在父进程 `wait` 回收前始终存活。
5. 只有 RUNNABLE 参与抽签；票数范围为 1–1000，因此每个持续就绪进程都有正概率。
6. 每核扫描候选时关闭本核中断；双核间以进程锁保护状态、票数及统计。选中候选后须再次持锁确认 RUNNABLE，才可改为 RUNNING，防止两个核重复派发同一进程。

`kill` 只设置标志并将睡眠进程变为就绪，让其在可安全检查的边界退出；直接从别的执行栈释放睡眠进程会破坏其内核栈和锁状态。调度器无就绪进程时打开中断并执行 `wfi`，否则时钟及 UART 无法唤醒进程。

## 发放测试与本地适配

官方 `support/tests/` 原文保留。内嵌用户程序的构建副本 `user/common.c` 将每 tick 睡眠的测试负载改为 CPU 忙循环：原循环几乎不累计 CPU tick，无法检验彩票份额。首个 `delta_cpu` 窗口从进程出生时的零计数计算，避免原框架在 stride 测试第一窗口固定返回零。 `user/lottery_test.c`、`user/stride_test.c` 用整数比例判断代替浮点运算，因为当前内核未启用用户 FPU；副本增加规范要求的 RESULT 行。 `stride_test` 属于分配到 ID 6/7 的策略测试，ID 5 不以其 PASS 为判据。

Lab3 自测使用的私有 `pagecount` 系统调用从 23 移到 26；23–25 按 Lab4 发放包给 `schedstat`、`time_rdtime`、`setickets`。 `support/tests/vmstress.c` 与 `user/vmstress.c` 因而同步更改调用号。

复现：

```sh
make -j4
python3 support/inject_uart.py --tree . --script support/tests/lab4-uart.script --log /tmp/lab4-uart.log
python3 support/inject_uart.py --tree . --script support/tests/lab3-uart.script --log /tmp/lab4-lab3.log
python3 support/inject_uart.py --tree . --script support/tests/lab2-uart-regression.script --log /tmp/lab4-lab2.log
```

验收目标是 `RESULT avg_ctx_switch`、`RESULT lottery_ratio`、`PASS: bench`、`PASS: lottery`、`VMSTRESS PASS`，以及 Lab3 页表 dump 和早期回归。脚本也运行 `stride_test` 作为额外的比例与活性检查；它并不能证明彩票策略具备步长策略的确定性有界公平性。

## 双核加分实现

`entry.S` 为 hart 0/1 分配独立的引导与调度器栈；hart 0 初始化内核页表、内存、进程及设备后发布启动标志，hart 1 再加载共享内核页表、配置本核 PLIC/时钟并进入调度循环。每核保存独立的当前进程、调度器 context、中断嵌套深度和随机数状态；trapframe 记录进程最近运行的 hart，使用户态陷入后仍在正确 CPU 上恢复。

自旋锁使用 RISC-V 原子交换，进入临界区前禁用本核中断。物理页位图、进程状态、管道引用计数及串口输入均有相应锁保护。退出进程持有自身进程锁跨过 `swtch`，直到调度器恢复并释放，父进程才可回收其内核栈。锁顺序主要为条件锁 → 进程锁；物理页锁只保护位图/计数，不在其内获取进程锁。

两个 CPU 同时运行两个持续就绪进程时，单纯在 RUNNABLE 集合抽签会让两个进程各占一个核，CPU 份额退化为 1:1。双核调度因而读取运行及就绪进程的累计 tick，以最高票数进程为参照，暂时跳过超过目标比例两个 tick 的候选。高票进程可持续占一核，低票进程按票数比间歇运行另一核；在 `60:30` 测试中观测到约 `294:150`。这会为维护份额有意留下部分空闲 CPU 容量，属于本策略在进程数少于核数时的取舍。

双核复现：

```sh
make -j4
python3 support/inject_uart.py --tree . --cpus 2 --script support/tests/lab4-smp-uart.script --log /tmp/lab4-smp.log
python3 support/inject_uart.py --tree . --cpus 2 --script support/tests/lab3-uart.script --log /tmp/lab4-smp-lab3.log
python3 support/inject_uart.py --tree . --cpus 2 --script support/tests/lab2-uart-regression.script --log /tmp/lab4-smp-lab2.log
```

`smpstress` 同时启动 6 个工作进程，每轮 300 次 `fork/exec/wait`，通过共享管道回传完成标记，并比较前后物理页数。双核脚本连续运行五轮该负载，再运行 `bench`、彩票测试和 `vmstress`。QEMU `-d int` 的一次 `bench` 追踪显示 hart 0/1 分别处理了 411/460 次用户 `ecall`，表明两个 hart 均实际执行了用户进程。

在三次独立双核启动中，每次五轮压力测试、`bench`、彩票测试及 `vmstress` 均通过，日志没有 `FAIL`、panic 或 kernel trap；Lab2/Lab3 双核回归及 Lab4 单核回归也通过。这些是有限压力测试结果，不能代替形式化并发证明或教师现场验收。
