# Lab1 设计笔记

学号：2024302111375  
实验：启动与串口输出

## 1. 个性化参数

| 参数                     | 值               | 本轮用途                    |
| ---------------------- | ---------------:| ----------------------- |
| `COURSE_SID`           | `2024302111375` | Banner 学号和派生值           |
| `LAB1_BANNER_PROTOCOL` | `1`             | 每个输出字符后追加 `.`           |
| `LAB1_STACK_KB`        | `8`             | 初始内核栈容量为 8KB            |
| `sid % 97`             | `68`            | Banner 输出为 `0x44`       |
| `sid % 16`             | `15`            | 节流周期为 `16 + 15 = 31` 字节 |

期望 Banner 的原始文本为：

```text
OSLAB1 sid=2024302111375 mod97=0x44
```

协议 1 的实际字节序列为每个字符后加一个点：

```text
O.S.L.A.B.1. .s.i.d.=.2.0.2.4.3.0.2.1.1.1.3.7.5. .m.o.d.9.7.=.0.x.4.4.
```

协议 1 不追加换行，最后一个字节是 `.`。

## 2. 启动时序

```text
QEMU virt 上电
  -> PC = 0x1000，执行 QEMU boot ROM
  -> -bios none 表示不加载传统 BIOS 固件
  -> QEMU 将 kernel ELF 按 program header 加载到 0x80000000
  -> 跳转到链接脚本指定的 _entry
  -> _entry 关闭 M 态中断
  -> 设置 mtvec 为对齐的 machine_trap
  -> hart 0 设置初始栈并调用 start
  -> 其他 hart 进入 park/wfi
  -> start 配置 M 态到 S 态切换所需 CSR
  -> mret
  -> S 态从 main 开始运行
  -> main 输出个性化 Banner
  -> main 进入 wfi 等待
```

### 2.1 为什么内核链接到 `0x80000000`

课程的 QEMU virt 内存布局规定 RAM 从物理地址 `0x80000000` 开始，QEMU 的 `-kernel` 会按照 ELF program header 把内核加载到这里。链接脚本中的：

```ld
BASE_ADDRESS = 0x80000000;
```

使 `_entry`、代码段、数据段和符号地址与实际加载地址一致。若改成 `0x0`，链接器生成的跳转和数据地址将与 QEMU 实际装载地址不一致，CPU 会从错误地址取指或访问错误数据。

`-bios none` 不是把内核加载到地址 0，而是跳过固件，由 QEMU 直接负责把内核放入 RAM 并从 boot ROM 转移到内核入口。

## 3. `entry.S` 逐步设计

### 3.1 关闭中断

第一条关键操作：

```asm
csrrci t0, mstatus, 0x8
```

`mstatus` 的 bit 3 是 `MIE`。启动早期还没有完成陷阱入口、特权级委托、PMP 和栈配置，若这时响应 M 态中断，可能跳入未准备好的路径。因此先清除 `MIE`。

### 3.2 设置机器态陷阱入口

```asm
la t0, machine_trap
csrw mtvec, t0
```

`mtvec` 保存 M 态陷阱入口。`machine_trap` 前使用：

```asm
.balign 4
```

保证入口地址满足 RISC-V 的 4 字节对齐要求。意外发生的早期 M 态陷阱会进入自旋，不会跳到无效地址。

### 3.3 处理多 hart

```asm
csrr t0, mhartid
bnez t0, park
```

Lab1 使用单核基线。只有 hart 0 进入内核初始化；其他 hart 在 `park` 中使用 `wfi` 等待。这样可以避免多个 hart 同时使用尚未设计为并发安全的启动栈、UART 和全局状态。

### 3.4 初始内核栈

```asm
la sp, stack0_top
```

栈在 `.bss.stack` 中静态分配：

```asm
.space LAB1_STACK_KB * 1024
```

本人的 `LAB1_STACK_KB` 为 8，因此容量为 8192 字节。栈从高地址向低地址增长，`sp` 初值必须指向 `stack0_top`，而不是 `stack0` 起始地址。

## 4. M 态到 S 态切换清单

`start()` 中完成以下配置：

1. 清除 `mstatus.MPP`；
2. 设置 `mstatus.MPP = S`；
3. 将 `mepc` 设置为 `main`；
4. 写 `medeleg = 0xffff`，将相关异常委托给 S 态；
5. 写 `mideleg = 0xffff`，将相关中断委托给 S 态；
6. 设置 PMP：

```c
w_pmpaddr0(0x3fffffffffffffull);
w_pmpcfg0(0xf);
```

第一行使用 NAPOT 形式覆盖物理地址空间；第二行使能 NAPOT，并打开读、写、执行权限。PMP 是必要条件：如果 S 态没有授权，`mret` 后执行第一条 S 态指令就可能产生 `cause = 1` 的 instruction access fault。

7. 写 `satp = 0`；
8. 执行 `sfence.vma`；
9. 设置 S 态设备中断使能位；
10. 执行 `mret`。

### 4.1 `satp` 选择 Bare 模式

Lab1 还没有页表分配器和页表建立逻辑，因此选择：

```c
w_satp(0);
```

这表示关闭地址转换，使用 Bare 模式。此时内核代码、数据和 UART MMIO 地址都按物理地址直接访问。恒等映射虽然理论上也能工作，但需要先建立页表，增加启动依赖，不适合本轮的最小启动链。

## 5. UART 轮询输出设计

QEMU virt 的 UART 基地址为：

```c
#define UART0 0x10000000L
```

本轮使用 16550 兼容寄存器：

| 寄存器 | 偏移  | 用途      |
| --- | ---:| ------- |
| THR | `0` | 发送保持寄存器 |
| LSR | `5` | 线路状态寄存器 |

发送一个字符的状态机：

```text
读取 UART0 + 5
  -> 检查 LSR bit 5，即 THRE
  -> THRE = 0：继续忙等
  -> THRE = 1：向 UART0 + 0 写入一个字节
  -> io_fence 保证 MMIO 写入顺序
```

本轮启动阶段没有中断驱动 UART，也没有超时机制；轮询等待 THRE 是简单且可靠的选择。

职责分层：

- `uartputc_sync`：只负责发送一个原始字节；
- `consoleputc`：给 `printf` 使用原始输出；
- `bannerputc`：实现学号个性化协议和 31 字节节流；
- `bannerputs`、`banner_number`、`banner_hex`：负责 Banner 组合。

## 6. `printf` 设计

实现的格式符：

| 格式符  | 行为                 |
| ---- | ------------------ |
| `%d` | 十进制有符号整数           |
| `%x` | 十六进制小写、无前导零        |
| `%p` | 按十六进制输出地址值         |
| `%s` | 字符串；空指针输出 `(null)` |
| `%c` | 单个字符               |
| `%%` | 输出 `%`             |

整数转换采用“低位入临时数组、逆序输出”。负数先输出 `-`，再转换绝对值；使用：

```c
(uint64)(-(value + 1)) + 1
```

避免对最小有符号整数直接取相反数造成溢出。

边界测试计划：

```text
printf("%d", 0)
printf("%d", -1)
printf("%d", 2147483647)
printf("%x", 0)
printf("%x", 0xffffffffffffffffULL)
printf("%s", "")
printf("%s", long_string)
printf("%s", (char *)0)
printf("%c", 'A')
printf("%%")
```

这些测试不应默认加入启动 Banner，否则会污染验收脚本逐字节比对的输出；可以在临时调试版本或单独测试入口中执行。

## 7. 故障定位方案

无输出时先运行：

```bash
qemu-system-riscv64 -machine virt -bios none \
  -kernel kernel/kernel -nographic \
  -d int -D int.log
```

重点检查：

- `cause = 1`：优先检查 PMP、入口地址、链接地址；
- 没有日志且没有输出：检查 `_entry`、`mepc`、`mtvec` 对齐；
- UART 卡死：检查 UART 基址、LSR 偏移和 THRE bit；
- 输出格式错误：检查协议是否每个字符后都追加 `.`，最后是否确实以 `.` 结束。

## 8. 输出结果

![](C:\Users\cyw\AppData\Roaming\marktext\images\2026-09-21-15-26-53-image.png)
