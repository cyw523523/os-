#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  struct stat st;
  int fd = open("/cutcreate", O_RDONLY);
  if (fd < 0) {
    printf("CREATED absent\n");
  } else {
    if (fstat(fd, &st) < 0 || st.type != T_FILE || st.size != 0 || st.nlink != 1)
      goto fail;
    close(fd);
    printf("CREATED empty-file\n");
  }
  fd = open("/cutwrite", O_RDONLY);
  if (fd >= 0) {
    char value[8] = {0};
    int n = read(fd, value, sizeof(value) - 1);
    if (fstat(fd, &st) < 0 || st.size != (uint64)n || st.nlink != 1 ||
        !((n == 3 && !memcmp(value, "OLD", 3)) ||
          (n == 6 && !memcmp(value, "NEW-V2", 6))))
      goto fail;
    close(fd);
    printf("CONTENT %s\n", value);
  }
  fd = open("/cutorphan", O_RDONLY);
  if (fd >= 0) {
    close(fd);
    printf("ORPHAN present\n");
  } else {
    printf("ORPHAN absent\n");
  }
  printf("FSVERIFY PASS\n");
  exit(0);
fail:
  printf("FSVERIFY FAIL\n");
  exit(1);
}
