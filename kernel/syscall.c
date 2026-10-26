#include "types.h"
#include "riscv.h"
#include "syscall.h"
#include "proc.h"

void consoleputc(int c);
int consolegetc(void);
void consolewait(void);
void dump_pagetable(pagetable_t root);

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
  int fd = (int)argraw(0);
  if (fd >= 3) return pipewrite(fd, va, (int)n);
  if (fd != 1 && fd != 2) return -1;
  if (n > 65536 || usercheck(current_proc->pagetable, va, n, 0) < 0)
    return -1;
  for (uint64 i = 0; i < n; i++) {
    char value;
    if (usercopy(current_proc->pagetable, va + i, &value, 1, 0) < 0)
      return -1;
    consoleputc(value);
  }
  return n;
}

static int
sys_read(void)
{
  uint64 va = argraw(1), n = argraw(2);
  int fd = (int)argraw(0);
  if (fd >= 3) return piperead(fd, va, (int)n);
  if (fd != 0) return -1;
  if (n > 65536 || usercheck(current_proc->pagetable, va, n, 1) < 0)
    return -1;
  for (uint64 i = 0; i < n; i++) {
    int c;
    while ((c = consolegetc()) < 0 && !current_proc->killed)
      consolewait();
    if (current_proc->killed) return -1;
    if (c == 0x10) {
      dump_pagetable(current_proc->pagetable);
      continue;
    }
    if (c == '\r')
      c = '\n';
    char value = c;
    if (usercopy(current_proc->pagetable, va + i, &value, 1, 1) < 0)
      return -1;
    consoleputc(c);
    if (c == '\n')
      return i + 1;
  }
  return n;
}

static int
sys_exec(void)
{
  char name[32];
  uint64 address = argraw(0);
  if (address >= MAXVA || sizeof(name) > MAXVA - address)
    return -1;
  int index;
  for (index = 0; index < sizeof(name) - 1; index++) {
    if (usercopy(current_proc->pagetable, address + index,
                 &name[index], 1, 0) < 0)
      return -1;
    if (name[index] == 0 || name[index] == '\n')
      break;
  }
  if (index == sizeof(name) - 1)
    return -1;
  name[index + 1] = 0;
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
  case SYS_close: ret = argraw(0) < 3 ? 0 : pipeclose((int)argraw(0)); break;
  case SYS_pipe: ret = pipealloc(argraw(0)); break;
  case SYS_kill: ret = proc_kill((int)argraw(0)); break;
  case SYS_pause: proc_pause((int)argraw(0)); ret = 0; break;
  case SYS_uptime: ret = proc_ticks(); break;
  case SYS_schedstat: ret = proc_schedstat((int)argraw(0), argraw(1), argraw(2)); break;
  case SYS_time_rdtime: ret = r_time(); break;
  case SYS_setickets: ret = proc_setickets((int)argraw(0)); break;
  case SYS_sbrk: ret = proc_sbrk((int)argraw(0)); break;
  case SYS_pagecount: ret = kpages_used(); break;
  default: ret = -1; break;
  }
  current_proc->trapframe->a0 = ret;
}
