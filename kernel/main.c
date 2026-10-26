#include "course_sid.h"
#include "types.h"
#include "riscv.h"

void bannerputs(const char *);
void bannerputc(int);
void bannernewline(void);
void consoleputc(int);
void consoleinit(void);
void procinit(void);
void kinit(void);
void kvminit(void);
void userinit(void);
void scheduler(void) __attribute__((noreturn));
void timerinit(void);
void consoleinithart(void);
uint64 kernel_satp(void);
extern char kernelvec[];
static int boot_ready;

static void
banner_number(uint64 value)
{
  char buf[32];
  int i = 0;

  do {
    buf[i++] = '0' + value % 10;
    value /= 10;
  } while (value != 0);

  while (i > 0)
    bannerputc(buf[--i]);
}

static void
banner_hex(uint64 value)
{
  static const char digits[] = "0123456789abcdef";
  char buf[16];
  int i = 0;

  do {
    buf[i++] = digits[value & 0xf];
    value >>= 4;
  } while (value != 0);

  bannerputs("0x");
  while (i > 0)
    bannerputc(buf[--i]);
}

void
main(void)
{
  if (r_tp() != 0) {
    while (!__atomic_load_n(&boot_ready, __ATOMIC_ACQUIRE))
      asm volatile("nop");
    w_satp(kernel_satp());
    sfence_vma();
    w_stvec((uint64)kernelvec);
    consoleinithart();
    timerinit();
    scheduler();
  }
  bannerputs("OSLAB1 sid=");
  banner_number(COURSE_SID);
  bannerputs(" mod97=");
  banner_hex(COURSE_SID % 97);
  bannernewline();
  consoleputc('\n');
  consoleinit();
  kinit();
  kvminit();
  procinit();
  w_stvec((uint64)kernelvec);
  timerinit();
  userinit();
  __atomic_store_n(&boot_ready, 1, __ATOMIC_RELEASE);
  scheduler();
}
