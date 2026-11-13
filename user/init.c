// Adopted children must be reaped even while the shell is waiting for input.
#include "kernel/types.h"
#include "user/user.h"

int main(void)
{
  for (;;) {
    int shell = fork();
    if (shell < 0) { pause(1); continue; }
    if (!shell) {
      char *argv[] = {"sh", 0};
      exec("sh", argv);
      exit(1);
    }
    int pid;
    do { pid = wait(0); } while (pid >= 0 && pid != shell);
  }
}
