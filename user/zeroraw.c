// Controlled demonstration: no stack stores between fork and exec/wait.
// The official C test builds argv on its stack and legitimately copies a page.
#include "lab5test.h"
#include "kernel/syscall.h"

#define STR1(x) #x
#define STR(x) STR1(x)
static const char name[] = "hi";
static const char *const args[] = {name, 0};

int main(void)
{
  long delta;
  asm volatile(
    "li a0, 0\n li a7, " STR(SYS_pmc) "\n ecall\n mv t0, a0\n"
    "li a7, " STR(SYS_fork) "\n ecall\n"
    "bltz a0, 2f\n bnez a0, 1f\n"
    "mv a0, %1\n mv a1, %2\n li a7, " STR(SYS_exec) "\n ecall\n"
    "li a0, 2\n li a7, " STR(SYS_exit) "\n ecall\n"
    "1: li a0, 0\n li a7, " STR(SYS_wait) "\n ecall\n"
    "bltz a0, 2f\n"
    "li a0, 0\n li a7, " STR(SYS_pmc) "\n ecall\n"
    "sub %0, a0, t0\n j 3f\n"
    "2: li %0, -1\n 3:\n"
    : "=&r"(delta) : "r"(name), "r"(args) : "a0", "a1", "a7", "t0", "memory");
  CHECK(delta == 0);
  printf("LAB5 ZERO PASS: fork-immediate-exec copies=%ld\n", delta);
  exit(0);
}
