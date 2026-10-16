#include "course_sid.h"
#include "types.h"

void bannerputs(const char *);
void bannerputc(int);
void bannernewline(void);
void consoleputc(int);
void consoleinit(void);
void procinit(void);
void kinit(void);
void kvminit(void);
void userinit(void) __attribute__((noreturn));

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
  userinit();
}
