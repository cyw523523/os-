#include "types.h"
#include "riscv.h"
#include "syscall.h"
#include "proc.h"
#include "fsinternal.h"
#include "buf.h"

uint64 sys_crash_at(void);

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

int
argint(int n, int *result)
{
  if (n < 0 || n > 2) return -1;
  *result = argraw(n);
  return 0;
}

static int
getpath(int n, char path[MAXPATH])
{
  return userstring(current_proc->pagetable, argraw(n), path, MAXPATH);
}

static int
sys_write(void)
{
  uint64 va = argraw(1), n = argraw(2);
  int fd = (int)argraw(0);
  if (n > 65536) return -1;
  if (fd >= 3) return fs_write(fd, va, (int)n);
  if (fd != 1 && fd != 2) return -1;
  if (usercheck(current_proc->pagetable, va, n, 0) < 0)
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
  if (n > 65536) return -1;
  if (fd >= 3) return fs_read(fd, va, (int)n);
  if (fd != 0) return -1;
  if (usercheck(current_proc->pagetable, va, n, 1) < 0)
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
  char name[MAXPATH];
  pagetable_t root = current_proc->pagetable;
  int length = getpath(0, name);
  if (length < 0) return -1;
  // The Lab2 shell includes the line terminator in the exec name.
  while (length && (name[length-1] == '\n' || name[length-1] == '\r'))
    name[--length] = 0;
  char *storage = kalloc();
  if (!storage) return -1;
  char *argv[LAB5_MAXARG + 1];
  uint64 array = argraw(1), used = 0;
  int argc = 0, result = -1;
  for (;;) {
    uint64 address;
    if (!array || array >= MAXVA ||
        (uint64)argc * sizeof(address) > MAXVA - array ||
        usercopy(root, array + argc * sizeof(address), &address,
                 sizeof(address), 0) < 0) break;
    if (!address) {
      argv[argc] = 0;
      result = proc_exec(name, argv, argc);
      break;
    }
    if (argc == LAB5_MAXARG || used == PGSIZE) break;
    int length = userstring(root, address, storage + used, PGSIZE - used);
    if (length < 0) break;
    argv[argc++] = storage + used;
    used += length + 1;
  }
  kfree(storage);
  return result;
}

static int
sys_pathop(int number)
{
  char path[MAXPATH], destination[MAXPATH];
  if (getpath(0, path) < 0) return -1;
  switch (number) {
  case SYS_open: return fs_open(path, (int)argraw(1));
  case SYS_mkdir: return fs_mkdir(path);
  case SYS_chdir: return fs_chdir(path);
  case SYS_unlink: return fs_unlink(path);
  case SYS_link:
    if (getpath(1, destination) >= 0) return fs_link(path, destination);
  }
  return -1;
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
  case SYS_close: ret = argraw(0) < 3 ? 0 : fs_close((int)argraw(0)); break;
  case SYS_open: case SYS_mkdir: case SYS_chdir: case SYS_link: case SYS_unlink:
    ret = sys_pathop(num);
    break;
  case SYS_fstat: ret = fs_fstat((int)argraw(0), argraw(1)); break;
  case SYS_dup: ret = fs_dup((int)argraw(0)); break;
  case SYS_sync: log_sync(); ret = 0; break;
  case SYS_crash_at: ret = sys_crash_at(); break;
  case SYS_crasharm: ret = log_arm_crash((int)argraw(0)); break;
  case SYS_fscachestat: bstats(); ret = 0; break;
  case SYS_pipe: ret = pipealloc(argraw(0)); break;
  case SYS_kill: ret = proc_kill((int)argraw(0)); break;
  case SYS_pause: proc_pause((int)argraw(0)); ret = 0; break;
  case SYS_uptime: ret = proc_ticks(); break;
  case SYS_schedstat: ret = proc_schedstat((int)argraw(0), argraw(1), argraw(2)); break;
  case SYS_time_rdtime: ret = r_time(); break;
  case SYS_setickets: ret = proc_setickets((int)argraw(0)); break;
  case SYS_sbrk: ret = proc_sbrk((int)argraw(0)); break;
  case SYS_pagecount: ret = kpages_used(); break;
  case SYS_pmc: ret = pmc_read((int)argraw(0)); break;
  default: ret = -1; break;
  }
  current_proc->trapframe->a0 = ret;
}
