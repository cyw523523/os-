#include "kernel/types.h"
#include "user/user.h"

static long
pagecount(void)
{
  register long result asm("a0");
  register long number asm("a7") = 27;
  asm volatile("ecall" : "=r"(result) : "r"(number) : "memory");
  return result;
}

int
main(void)
{
  long before = pagecount();
  char *boundary = sbrk(4096);
  if (boundary == (char *)-1) {
    printf("VMSTRESS FAIL: boundary allocation\n");
    exit(1);
  }
  if (write(1, boundary + 4095, 2) != -1 ||
      read(0, (void *)0, 1) != -1) {
    printf("VMSTRESS FAIL: invalid user pointer accepted\n");
    exit(1);
  }
  if (sbrk(-4096) != boundary + 4096) {
    printf("VMSTRESS FAIL: heap shrink\n");
    exit(1);
  }
  int guard_pid = fork();
  if (guard_pid == 0) {
    uint64 stack_pointer;
    asm volatile("mv %0, sp" : "=r"(stack_pointer));
    volatile char *guard = (volatile char *)((stack_pointer & ~4095ULL) - 4096);
    *guard = 1;
    exit(0);
  }
  int guard_status = 0;
  if (guard_pid < 0 || wait(&guard_status) != guard_pid || guard_status == 0) {
    printf("VMSTRESS FAIL: guard page\n");
    exit(1);
  }
  long stable = pagecount();
  if (sbrk(128 * 1024 * 1024) != (char *)-1 || pagecount() != stable) {
    printf("VMSTRESS FAIL: OOM rollback\n");
    exit(1);
  }
  for (int round = 0; round < 200; round++) {
    int pid = fork();
    if (pid < 0) {
      printf("VMSTRESS FAIL: fork at %d\n", round);
      exit(1);
    }
    if (pid == 0) {
      char *arguments[] = {"noop", 0};
      exec("noop", arguments);
      exit(1);
    }
    int status = -1;
    if (wait(&status) != pid || status != 0) {
      printf("VMSTRESS FAIL: wait at %d\n", round);
      exit(1);
    }
  }
  long after = pagecount();
  if (stable == after)
    printf("VMSTRESS PASS: 200 fork-exec cycles; pages=%d\n", (int)after);
  else
    printf("VMSTRESS FAIL: pages before=%d after=%d\n", (int)stable, (int)after);
  exit(stable == after && after >= before ? 0 : 1);
}
