#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void
check(int ok)
{
  if (ok) return;
  printf("FSCRASH FAIL\n");
  exit(1);
}

int
main(void)
{
  char input[8];
  printf("FSCRASH stage and mode (c=create, w=write, o=orphan):\n");
  gets(input, sizeof(input));
  check(input[0] >= '0' && input[0] <= '7');
  int stage = input[0] - '0';
  if (input[1] == 'c') {
    check(crasharm(stage) == 0);
    open("/cutcreate", O_CREATE | O_WRONLY);
  } else if (input[1] == 'w') {
    int fd = open("/cutwrite", O_CREATE | O_WRONLY);
    check(fd >= 0 && write(fd, "OLD", 3) == 3 && close(fd) == 0);
    fd = open("/cutwrite", O_WRONLY);
    check(fd >= 0 && sync() == 0 && crasharm(stage) == 0);
    write(fd, "NEW-V2", 6);
  } else if (input[1] == 'o') {
    int fd = open("/cutorphan", O_CREATE | O_WRONLY);
    check(fd >= 0 && write(fd, "ORPHAN", 6) == 6 && sync() == 0);
    check(crasharm(stage) == 0);
    unlink("/cutorphan");
  }
  printf("FSCRASH FAIL no reset\n");
  exit(1);
}
