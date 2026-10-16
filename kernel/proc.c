#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "proc.h"

struct uprog { uint64 start, end; char name[8]; };
extern struct uprog _uprog_table[];
extern char uservec[], userret[], stack0_top[];
void usertrap(void);

static struct proc procs[NPROC];
struct proc *current_proc;
static int nextpid = 1;

static int
streq(const char *left, const char *right)
{
  while (*left && *left == *right)
    left++, right++;
  return (*left == 0 || *left == '\n' || *left == '\r') && *right == 0;
}

static void
release(struct proc *process)
{
  proc_freepagetable(process->pagetable);
  if (process->trapframe)
    kfree(process->trapframe);
  process->pagetable = 0;
  process->trapframe = 0;
  process->state = UNUSED;
}

static struct proc *
allocproc(void)
{
  for (int index = 0; index < NPROC; index++) {
    struct proc *process = &procs[index];
    if (process->state != UNUSED)
      continue;
    process->state = RUNNABLE;
    process->pid = nextpid++;
    process->parent = 0;
    process->size = 0;
    process->image_size = 0;
    process->trapframe = kalloc();
    if (process->trapframe)
      process->pagetable = proc_pagetable(process);
    if (process->pagetable)
      return process;
    release(process);
    return 0;
  }
  return 0;
}

static int
load(struct proc *process, const char *name)
{
  for (struct uprog *program = _uprog_table; program->start;
       program = (struct uprog *)((program->end + 7) & ~7ULL)) {
    if (!streq(name, program->name))
      continue;
    uint64 image_size = program->end - program->start;
    uint64 image_end = PGROUNDUP(PGSIZE + image_size);
    uint64 guard_start = USER_STACK_BASE - LAB3_GUARD_PAGES * PGSIZE;
    if (image_end >= guard_start)
      return -1;
    pagetable_t replacement = proc_pagetable(process);
    if (replacement == 0)
      return -1;
    if (uvmalloc(replacement, PGSIZE, image_end, PTE_R | PTE_W | PTE_X) < 0 ||
        uvmalloc(replacement, USER_STACK_BASE, USER_STACK_BASE + PGSIZE,
                 PTE_R | PTE_W) < 0 ||
        usercopy(replacement, PGSIZE, (void *)program->start, image_size, 1) < 0) {
      proc_freepagetable(replacement);
      return -1;
    }
    pagetable_t old = process->pagetable;
    process->pagetable = replacement;
    process->image_size = image_size;
    process->size = image_end;
    process->trapframe->epc = PGSIZE;
    process->trapframe->sp = USER_STACK_BASE + PGSIZE - 16;
    proc_freepagetable(old);
    return 0;
  }
  return -1;
}

void
procinit(void)
{
  for (int index = 0; index < NPROC; index++)
    procs[index].state = UNUSED;
}

void
switch_to(struct proc *process)
{
  current_proc = process;
  process->state = RUNNING;
  process->trapframe->kernel_satp = kernel_satp();
  process->trapframe->kernel_sp = (uint64)stack0_top;
  process->trapframe->kernel_trap = (uint64)usertrap;
  process->trapframe->kernel_hartid = 0;
  uint64 status = r_sstatus();
  status &= ~SSTATUS_SPP;
  status |= SSTATUS_SPIE;
  w_sstatus(status);
  w_sepc(process->trapframe->epc);
  w_stvec(TRAMPOLINE + ((uint64)uservec & (PGSIZE - 1)));
  ((void (*)(uint64))(TRAMPOLINE + ((uint64)userret & (PGSIZE - 1))))
    (proc_satp(process));
  for (;;) ;
}

void
userinit(void)
{
  struct proc *process = allocproc();
  if (process == 0 || load(process, "sh") < 0)
    for (;;) asm volatile("wfi");
  dump_pagetable(process->pagetable);
  switch_to(process);
}

int
proc_fork(void)
{
  struct proc *child = allocproc();
  if (child == 0)
    return -1;
  if (uvmcopy(current_proc->pagetable, child->pagetable, 0,
              current_proc->size) < 0 ||
      uvmcopy(current_proc->pagetable, child->pagetable,
              USER_STACK_BASE, USER_STACK_BASE + PGSIZE) < 0) {
    release(child);
    return -1;
  }
  uchar *source = (uchar *)current_proc->trapframe;
  uchar *target = (uchar *)child->trapframe;
  for (uint64 index = 0; index < sizeof(*child->trapframe); index++)
    target[index] = source[index];
  child->trapframe->a0 = 0;
  child->parent = current_proc;
  child->image_size = current_proc->image_size;
  child->size = current_proc->size;
  return child->pid;
}

int
proc_exec(const char *name)
{
  return load(current_proc, name);
}

uint64
proc_sbrk(int amount)
{
  struct proc *process = current_proc;
  uint64 old = process->size;
  uint64 limit = USER_STACK_BASE - LAB3_GUARD_PAGES * PGSIZE;
  if (amount < 0) {
    uint64 decrease = -(long)amount;
    uint64 minimum = PGROUNDUP(PGSIZE + process->image_size);
    if (decrease > old - minimum)
      return -1;
    process->size = old - decrease;
    uvmdealloc(process->pagetable, process->size, old);
    sfence_vma();
    return old;
  }
  if ((uint64)amount > limit - old)
    return -1;
  uint64 next = old + amount;
  if (uvmalloc(process->pagetable, old, next, PTE_R | PTE_W) < 0)
    return -1;
  process->size = next;
  return old;
}

int
proc_wait(uint64 status_addr)
{
  for (int index = 0; index < NPROC; index++) {
    struct proc *process = &procs[index];
    if (process->parent == current_proc && process->state == ZOMBIE) {
      int pid = process->pid;
      int status = process->exit_status;
      if (status_addr && usercopy(current_proc->pagetable, status_addr,
                                  &status, sizeof(status), 1) < 0)
        return -1;
      release(process);
      return pid;
    }
  }
  for (int index = 0; index < NPROC; index++) {
    if (procs[index].parent == current_proc && procs[index].state == RUNNABLE) {
      current_proc->state = WAITING;
      switch_to(&procs[index]);
    }
  }
  return -1;
}

void
proc_exit(int status)
{
  struct proc *process = current_proc;
  process->exit_status = status;
  process->state = ZOMBIE;
  if (process->parent && process->parent->state == WAITING) {
    struct proc *parent = process->parent;
    int pid = process->pid;
    if (parent->trapframe->a0 &&
        usercopy(parent->pagetable, parent->trapframe->a0,
                 &status, sizeof(status), 1) < 0)
      pid = -1;
    parent->trapframe->a0 = pid;
    release(process);
    switch_to(parent);
  }
  for (;;) asm volatile("wfi");
}
