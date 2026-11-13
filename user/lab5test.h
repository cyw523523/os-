#include "kernel/types.h"
#include "kernel/lab5.h"
#include "user/user.h"

#define CHECK(x) do { if (!(x)) { \
  printf("LAB5 FAIL line=%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static inline long pagecount(void)
{
  register long result asm("a0");
  register long number asm("a7") = SYS_pagecount;
  asm volatile("ecall" : "=r"(result) : "r"(number) : "memory");
  return result;
}

static inline void reap(int pid, int expected)
{
  int status = 99;
  CHECK(pid > 0 && wait(&status) == pid && status == expected);
}
