#include "lab5test.h"

int main(int argc, char **argv)
{
  CHECK(argv && argv[argc] == 0 && ((uint64)argv & 15) == 0);
  if (argc == 0) exit(0);
  CHECK(argc == 4 && strcmp(argv[0], "argcheck") == 0);
  CHECK(strcmp(argv[1], "alpha") == 0 && argv[2][0] == 0);
  CHECK(strlen(argv[3]) == 79);
  for (int i = 0; i < 79; i++) CHECK(argv[3][i] == 'q');
  printf("LAB5 ARGV PASS: argc=4 strings/null/alignment\n");
  exit(0);
}
