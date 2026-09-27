#include "types.h"
#include "riscv.h"
#include "syscall.h"
#include "proc.h"

void consoleputc(int c);
int consolegetc(void);

static uint64
argraw(int n)
{
  struct trapframe *tf = current_proc->trapframe;
  switch (n) {
  case 0: return tf->a0;
  case 1: return tf->a1;
  case 2: return tf->a2;
  default: return 0;
  }
}

static int
sys_write(void)
{
  uint64 va = argraw(1), n = argraw(2);
  char *p = useraddr(current_proc, va, n);
  if (p == 0)
    return -1;
  for (uint64 i = 0; i < n; i++)
    consoleputc(p[i]);
  return n;
}

static int
sys_read(void)
{
  uint64 va = argraw(1), n = argraw(2);
  char *p = useraddr(current_proc, va, n);
  if (p == 0)
    return -1;
  intr_on();
  for (uint64 i = 0; i < n; i++) {
    int c;
    while ((c = consolegetc()) < 0)
      asm volatile("wfi");
    if (c == '\r')
      c = '\n';
    p[i] = c;
    consoleputc(c);
    if (c == '\n')
      return intr_off(), i + 1;
  }
  intr_off();
  return n;
}

static int
sys_exec(void)
{
  char *name = useraddr(current_proc, argraw(0), 1);
  if (name == 0)
    return -1;
  return proc_exec(name);
}

void
syscall(void)
{
  int num = current_proc->trapframe->a7;
  long ret = -1;
  switch (num) {
  case SYS_fork: ret = proc_fork(); break;
  case SYS_exit: proc_exit((int)argraw(0));
  case SYS_wait: ret = proc_wait(argraw(0)); break;
  case SYS_read: ret = sys_read(); break;
  case SYS_exec: ret = sys_exec(); break;
  case SYS_getpid: ret = current_proc->pid; break;
  case SYS_write: ret = sys_write(); break;
  case SYS_close: ret = 0; break;
  default: ret = -1; break;
  }
  current_proc->trapframe->a0 = ret;
}
