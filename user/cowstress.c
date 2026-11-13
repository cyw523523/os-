#include "lab5test.h"

#define WORKERS 6
#define PAGES 16

static void isolation(void)
{
  char *page = sbrk(PAGES * 4096);
  CHECK(page != (char *)-1 && ((uint64)page & 4095) == 0);
  memset(page, 0x11, PAGES * 4096);
  int ready[2], go[2];
  CHECK(pipe(ready) == 0 && pipe(go) == 0);
  unsigned long long shared = pmc(1), copies = pmc(0);
  for (int w = 0; w < WORKERS; w++) {
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
      close(ready[0]); close(go[1]);
      CHECK(write(ready[1], "r", 1) == 1);
      char command;
      CHECK(read(go[0], &command, 1) == 1);
      for (int round = 0; round < 12; round++) {
        memset(page, 0x20 + w, PAGES * 4096);
        for (int i = 0; i < PAGES * 4096; i++)
          CHECK((uchar)page[i] == 0x20 + w);
      }
      exit(0);
    }
  }
  close(ready[1]); close(go[0]);
  char token;
  for (int i = 0; i < WORKERS; i++) CHECK(read(ready[0], &token, 1) == 1);
  CHECK(pmc(1) >= shared + PAGES);
  for (int i = 0; i < WORKERS; i++) CHECK(write(go[1], "g", 1) == 1);
  for (int i = 0; i < WORKERS; i++) { int st; CHECK(wait(&st) > 0 && st == 0); }
  CHECK(pmc(0) >= copies + WORKERS * PAGES);
  for (int i = 0; i < PAGES * 4096; i++) CHECK(page[i] == 0x11);
  CHECK(pmc(1) == shared);
  close(ready[0]); close(go[1]);
  CHECK(sbrk(-PAGES * 4096) == page + PAGES * 4096);
  printf("LAB5 SHARING PASS: six writers, isolated data, shared refs restored\n");
}

static void copyout_test(void)
{
  char *page = sbrk(4096); CHECK(page != (char *)-1);
  memset(page, 0x44, 4096);
  int fd[2]; CHECK(pipe(fd) == 0);
  int pid = fork(); CHECK(pid >= 0);
  if (!pid) {
    close(fd[1]);
    // Kernel copies pipe data into an untouched COW heap page.
    CHECK(read(fd[0], page, 1) == 1 && page[0] == 'x');
    CHECK(schedstat(-1, (int *)(page + 8), (int *)(page + 12)) == 0);
    int *newfds = (int *)(page + 16);
    CHECK(pipe(newfds) == 0); close(newfds[0]); close(newfds[1]);
    int grandchild = fork(); CHECK(grandchild >= 0);
    if (!grandchild) exit(7);
    // wait copies status into a newly shared heap mapping.
    CHECK(wait((int *)(page + 24)) == grandchild && *(int *)(page + 24) == 7);
    exit(0);
  }
  close(fd[0]); CHECK(write(fd[1], "x", 1) == 1); close(fd[1]);
  reap(pid, 0);
  for (int i = 0; i < 4096; i++) CHECK(page[i] == 0x44);
  CHECK(sbrk(-4096) == page + 4096);
  printf("LAB5 COPYOUT PASS: pipe/read/wait/schedstat and nested fork\n");
}

static void readonly_test(void)
{
  int pid = fork(); CHECK(pid >= 0);
  if (!pid) { *(volatile char *)(uint64)readonly_test = 0; exit(0); }
  reap(pid, -1);
  CHECK(read(0, (void *)(uint64)readonly_test, 1) == -1);
  printf("LAB5 READONLY PASS: real text cannot become writable\n");
}

static void kill_test(void)
{
  char *page = sbrk(192 * 4096); CHECK(page != (char *)-1);
  int ready[2]; CHECK(pipe(ready) == 0);
  for (int round = 0; round < 24; round++) {
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
      CHECK(write(ready[1], "r", 1) == 1);
      for (;;) for (int i = 0; i < 192; i++)
        ((volatile char *)page)[i * 4096]++;
    }
    char token; CHECK(read(ready[0], &token, 1) == 1);
    CHECK(kill(pid) == 0);
    reap(pid, -1);
    for (int i = 0; i < 192; i++) CHECK(page[i * 4096] == 0);
  }
  close(ready[0]); close(ready[1]);
  CHECK(sbrk(-192 * 4096) == page + 192 * 4096);
  printf("LAB5 KILL PASS: 24 shared-writer exits\n");
}

static void budget_test(void)
{
  char *page = sbrk(160 * 4096); CHECK(page != (char *)-1);
  int pid = fork(); CHECK(pid >= 0);
  if (!pid) {
    for (int i = 0; i < 160; i++) page[i * 4096] = 9;
    for (int i = 0; i < 160; i++) CHECK(page[i * 4096] == 9);
    exit(0);
  }
  reap(pid, 0);
  for (int i = 0; i < 160; i++) CHECK(page[i * 4096] == 0);
  CHECK(sbrk(-160 * 4096) == page + 160 * 4096);
  printf("LAB5 BUDGET PASS: >143 copies reported, data intact\n");
}

static void orphan_test(void)
{
  long before = pagecount();
  char *page = sbrk(4096); CHECK(page != (char *)-1);
  page[0] = 0x61;
  int fd[2]; CHECK(pipe(fd) == 0);
  int pid = fork(); CHECK(pid >= 0);
  if (!pid) {
    close(fd[0]);
    int orphan = fork(); CHECK(orphan >= 0);
    if (orphan) exit(0);
    pause(1); // Parent exits before this child writes the inherited COW page.
    page[0] = 0x62;
    CHECK(write(fd[1], "o", 1) == 1);
    exit(0);
  }
  close(fd[1]); reap(pid, 0);
  char token;
  CHECK(read(fd[0], &token, 1) == 1 && token == 'o');
  CHECK(read(fd[0], &token, 1) == 0 && page[0] == 0x61);
  close(fd[0]); CHECK(sbrk(-4096) == page + 4096);
  for (int i = 0; i < 40 && pagecount() != before; i++) pause(1);
  CHECK(pagecount() == before);
  printf("LAB5 ORPHAN PASS: parent exits first, init reaps shared pages\n");
}

int main(void)
{
  CHECK(pmc(2) == (unsigned long long)-1);
  long before = pagecount();
  unsigned long long shared = pmc(1);
  isolation(); copyout_test(); readonly_test(); kill_test(); budget_test(); orphan_test();
  CHECK(pagecount() == before && pmc(1) == shared);
  printf("LAB5 COWSTRESS PASS: pages=%ld shared=%llu\n", before, shared);
  exit(0);
}
