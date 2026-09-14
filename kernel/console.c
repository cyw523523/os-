#include "types.h"
#include "riscv.h"
#include "course_sid.h"
#include "memlayout.h"

#define UART_THR 0
#define UART_LSR 5
#define UART_LSR_THRE 0x20

static uint64 banner_bytes;

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
