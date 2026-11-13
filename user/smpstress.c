#include "kernel/types.h"
#include "user/user.h"

#define WORKERS 6
#define ROUNDS 50

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
  int fd[2];
  if (pipe(fd) < 0) {
    printf("SMPSTRESS FAIL: pipe\n");
    exit(1);
  }
  for (int worker = 0; worker < WORKERS; worker++) {
    int pid = fork();
    if (pid < 0) {
      printf("SMPSTRESS FAIL: worker fork\n");
      exit(1);
    }
    if (pid == 0) {
      close(fd[0]);
      for (int n = 0; n < ROUNDS; n++) {
        int child;
        while ((child = fork()) < 0) pause(1);
        if (child == 0) {
          char *argv[] = {"noop", 0};
          exec("noop", argv);
          exit(2);
        }
        int status = -1;
        if (wait(&status) != child || status != 0 ||
            write(fd[1], "x", 1) != 1)
          exit(3);
      }
      close(fd[1]);
      exit(0);
    }
  }
  close(fd[1]);
  int count = 0;
  char byte;
  while (count < WORKERS * ROUNDS && read(fd[0], &byte, 1) == 1)
    count++;
  close(fd[0]);
  int good = count == WORKERS * ROUNDS;
  for (int i = 0; i < WORKERS; i++) {
    int status = -1;
    if (wait(&status) < 0 || status != 0) good = 0;
  }
  long after = pagecount();
  if (!good || before != after) {
    printf("SMPSTRESS FAIL: count=%d pages=%ld:%ld\n",
           count, before, after);
    exit(1);
  }
  printf("SMPSTRESS PASS: workers=%d forks=%d pages=%ld\n",
         WORKERS, WORKERS * ROUNDS, after);
  exit(0);
}
