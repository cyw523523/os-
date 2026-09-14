#include "types.h"
#include <stdarg.h>

void consoleputc(int c);

static void
printint(long long value, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[sizeof(long long) * 8 + 1];
  uint64 x;
  int i = 0;

  if (sign && value < 0) {
    consoleputc('-');
    x = (uint64)(-(value + 1)) + 1;
  } else {
    x = (uint64)value;
  }

  do {
    buf[i++] = digits[x % base];
    x /= base;
  } while (x != 0);

  while (--i >= 0)
    consoleputc(buf[i]);
}

void
printf(const char *fmt, ...)
{
  va_list ap;
  int c;
  const char *s;

  va_start(ap, fmt);
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      consoleputc((uchar)*fmt);
      continue;
    }

    fmt++;
    if (*fmt == '\0')
      break;

    c = *fmt;
    switch (c) {
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
    case 'p':
      printint(va_arg(ap, uint64), 16, 0);
      break;
    case 's':
      s = va_arg(ap, const char *);
      if (s == 0)
        s = "(null)";
      while (*s)
        consoleputc((uchar)*s++);
      break;
    case 'c':
      consoleputc(va_arg(ap, int));
      break;
    case '%':
      consoleputc('%');
      break;
    default:
      consoleputc('%');
      consoleputc(c);
      break;
    }
  }
  va_end(ap);
}
