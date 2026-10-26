#include "types.h"
#include "riscv.h"
#include "course_sid.h"
#include "memlayout.h"
#include "proc.h"

#define UART_THR 0
#define UART_IER 1
#define UART_LSR 5
#define UART_LSR_THRE 0x20
#define UART_LSR_DR 0x01

static uint64 banner_bytes;
static struct {
  struct spinlock lock;
  uchar data[LAB2_BUF_SIZE];
  uint64 read;
  uint64 write;
} input;

void
consoleinit(void)
{
  volatile uchar *uart = (volatile uchar *)UART0;

  input.read = input.write = 0;
  uart[UART_IER] = 0x1;

  *(volatile uint32 *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;
  *(volatile uint32 *)PLIC_SENABLE(0) |= 1U << UART0_IRQ;
  *(volatile uint32 *)PLIC_SPRIORITY(0) = 0;
  io_fence();
}

static void
uartputc_sync(int c)
{
  volatile uchar *uart = (volatile uchar *)UART0;

  while ((uart[UART_LSR] & UART_LSR_THRE) == 0)
    ;
  uart[UART_THR] = (uchar)c;
  io_fence();
}

static void
throttle(void)
{
  uint64 i;
  uint64 rounds = (COURSE_SID % 7) + 1;

  for (i = 0; i < rounds; i++)
    asm volatile("nop");
}

void
consoleputc(int c)
{
  uartputc_sync(c);
}

int
consolegetc(void)
{
  acquire(&input.lock);
  if (input.read == input.write)
    return release_lock(&input.lock), -1;
  int c = input.data[input.read++ % LAB2_BUF_SIZE];
  release_lock(&input.lock);
  return c;
}

void
consolewait(void)
{
  acquire(&input.lock);
  while (input.read == input.write && !current_proc->killed)
    sleep(&input, &input.lock);
  release_lock(&input.lock);
}

void
consoleintr(void)
{
  volatile uchar *uart = (volatile uchar *)UART0;
  acquire(&input.lock);

  while (uart[UART_LSR] & UART_LSR_DR) {
    int c = uart[UART_THR];
    if (input.write - input.read < LAB2_BUF_SIZE)
      input.data[input.write++ % LAB2_BUF_SIZE] = c;
  }
  wakeup(&input);
  release_lock(&input.lock);
}

void
bannerputc(int c)
{
  uartputc_sync(c);
  banner_bytes++;

  if (LAB1_BANNER_PROTOCOL == 1)
    uartputc_sync('.');

  if (banner_bytes == (16 + COURSE_SID % 16)) {
    throttle();
    banner_bytes = 0;
  }
}

void
bannerputs(const char *s)
{
  while (*s != '\0')
    bannerputc((uchar)*s++);
}

void
bannernewline(void)
{
  if (LAB1_BANNER_PROTOCOL == 0)
    bannerputc('\n');
}
