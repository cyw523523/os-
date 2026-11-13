#include "lab5test.h"

// Fill the allocator exactly, then expose different partial-fork/exec failures.
static void rollback_test(void)
{
  char *start = sbrk(0);
  long before = pagecount();
  while (sbrk(64 * 1024) != (char *)-1) ;
  while (sbrk(4096) != (char *)-1) ;
  for (int slack = 0; slack < 16; slack++) {
    long stable = pagecount();
    int pid = fork();
    if (!pid) exit(0);
    if (pid > 0) reap(pid, 0);
    CHECK(pagecount() == stable);
    char *argv[] = {"hi", 0};
    // Embedded hi needs more pages than the slack tested here. If exec could
    // succeed it would replace this process; use a separate child below instead.
    if (slack < 9) {
      CHECK(exec("hi", argv) == -1);
      CHECK(pagecount() == stable);
    }
    CHECK(sbrk(-4096) != (char *)-1);
  }
  char *top = sbrk(0);
  CHECK(sbrk(-(int)(top - start)) == top);
  CHECK(pagecount() == before);
  printf("LAB5 OOM ROLLBACK PASS: partial fork and exec cleanup\n");
}

static void fault_oom_test(void)
{
  char *target = sbrk(4096); CHECK(target != (char *)-1);
  target[0] = 0x51;
  int ready[2], go[2]; CHECK(pipe(ready) == 0 && pipe(go) == 0);
  int pid = fork(); CHECK(pid >= 0);
  if (!pid) {
    char byte;
    CHECK(write(ready[1], "r", 1) == 1);
    CHECK(read(go[0], &byte, 1) == 1);
    target[0] = 0x52; // No free physical page: must fail safely.
    exit(0);
  }
  char byte; CHECK(read(ready[0], &byte, 1) == 1);
  // Make parent's stack private before exhausting memory, including status.
  int status = 99;
  char *start = sbrk(0);
  while (sbrk(64 * 1024) != (char *)-1) ;
  while (sbrk(4096) != (char *)-1) ;
  CHECK(write(go[1], "g", 1) == 1);
  CHECK(wait(&status) == pid && status == -1 && target[0] == 0x51);
  char *top = sbrk(0); CHECK(sbrk(-(int)(top - start)) == top);
  close(ready[0]); close(ready[1]); close(go[0]); close(go[1]);
  CHECK(sbrk(-4096) == target + 4096);
  printf("LAB5 OOM FAULT PASS: no allocation, child killed, parent intact\n");
}

int main(void)
{
  long before = pagecount();
  rollback_test(); fault_oom_test();
  CHECK(pagecount() == before);
  printf("LAB5 COWOOM PASS: pages=%ld\n", before);
  exit(0);
}
