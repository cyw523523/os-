#include "lab5test.h"

int main(void)
{
  char *mem = sbrk(8192);
  CHECK(mem != (char *)-1);
  char *cross = mem + 4096 - 17;
  memset(cross, 'q', 79); cross[79] = 0;
  char *argv[] = {"argcheck", "alpha", "", cross, 0};
  int pid = fork();
  if (!pid) { exec("argcheck", argv); exit(2); }
  reap(pid, 0);
  char *empty[] = {0};
  pid = fork();
  if (!pid) { exec("argcheck", empty); exit(2); }
  reap(pid, 0);
  long before = pagecount();
  for (int i = 0; i < 32; i++) {
    CHECK(exec("missing-program", argv) == -1);
    CHECK(exec((char *)0, argv) == -1);
    CHECK(exec("argcheck", (char **)0) == -1);
    CHECK(exec("argcheck", (char **)0x3ffffffff8ULL) == -1);
    char *bad[] = {(char *)0x3fffffffffULL, 0};
    CHECK(exec("argcheck", bad) == -1);
    char *many[34];
    for (int n = 0; n < 33; n++) many[n] = "x";
    many[33] = 0;
    CHECK(exec("argcheck", many) == -1);
    memset(mem, 'x', 4096);
    char *longarg[] = {mem, 0};
    CHECK(exec("argcheck", longarg) == -1);
    // Fits the staging page but leaves no room for stack argv/alignment.
    mem[4095] = 0;
    CHECK(exec("argcheck", longarg) == -1);
    CHECK(pagecount() == before);
  }
  CHECK(sbrk(-8192) == mem + 8192);
  printf("LAB5 EXEC PASS: cross-page argv, empty argv, errors, rollback\n");
  exit(0);
}
