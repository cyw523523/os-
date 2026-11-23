#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "user/user.h"

static char data[BSIZE], got[BSIZE];

static void
check(int ok, const char *what)
{
  if (ok) return;
  printf("FSCHECK FAIL %s\n", what);
  exit(1);
}

static void
fill(int block)
{
  for (int i = 0; i < BSIZE; i++) data[i] = block + i % 113;
}

int
main(void)
{
  check(mkdir("/lab6dir") == 0 && mkdir("/lab6dir/sub") == 0 &&
        mkdir("/lab6dir/sub/deep") == 0, "mkdir");
  check(chdir("/lab6dir/sub/deep") == 0 && chdir("./../deep/.") == 0, "chdir");
  check(unlink("/lab6dir") < 0, "nonempty dir");
  int fd = open("./../../sub/deep/data", O_RDWR | O_CREATE);
  check(fd >= 0, "create through nested path");
  for (int block = 0; block < NDIRECT + 8; block++) {
    fill(block);
    check(write(fd, data, sizeof(data)) == sizeof(data), "indirect write");
  }
  check(close(fd) == 0, "close after write");
  fd = open("data", O_RDONLY);
  int copy = dup(fd);
  struct stat st;
  check(fd >= 0 && copy >= 0, "reopen and dup");
  check(link("data", "alias") == 0 && fstat(fd, &st) == 0 &&
        st.nlink == 2 && st.size == (NDIRECT + 8) * BSIZE, "link count");
  check(unlink("data") == 0 && unlink("alias") == 0 &&
        open("data", O_RDONLY) < 0 && fstat(fd, &st) == 0 && st.nlink == 0,
        "unlink while open");
  for (int block = 0; block < NDIRECT + 8; block++) {
    fill(block);
    check(read(block % 2 ? copy : fd, got, sizeof(got)) == sizeof(got) &&
          memcmp(data, got, sizeof(got)) == 0, "shared offset and indirect read");
  }
  check(read(fd, got, 1) == 0 && close(copy) == 0 && close(fd) == 0, "EOF and reclaim");
  check(chdir("/") == 0 && unlink("lab6dir/sub/deep") == 0 &&
        unlink("lab6dir/sub") == 0 && unlink("lab6dir") == 0, "remove directories");
  check(fscachestat() == 0, "cache invariants");
  printf("FSCHECK PASS\n");
  exit(0);
}
