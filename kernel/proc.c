#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc.h"

struct uprog {
  uint64 start;
  uint64 end;
  char name[8];
};

extern struct uprog _uprog_table[];
extern char trampoline[], uservec[], userret[];
void usertrap(void);

static struct proc procs[NPROC];
static uchar trapframe_pages[NPROC][PGSIZE] __attribute__((aligned(PGSIZE)));
static uchar usermem[NPROC][USER_MEM_SIZE] __attribute__((aligned(PGSIZE)));
extern char stack0_top[];
struct proc *current_proc;
static int nextpid = 1;

static void
copybytes(void *dst, const void *src, uint64 n)
{
  uchar *d = dst;
  const uchar *s = src;
  while (n--)
    *d++ = *s++;
}

static void
clearbytes(void *dst, uint64 n)
{
  uchar *d = dst;
  while (n--)
    *d++ = 0;
}

static int
streq(const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return (*a == '\0' || *a == '\n' || *a == '\r') && *b == '\0';
}

static struct proc *
allocproc(void)
{
  for (int i = 0; i < NPROC; i++) {
    if (procs[i].state == UNUSED) {
      struct proc *p = &procs[i];
      clearbytes(p, sizeof(*p));
      clearbytes(trapframe_pages[i], PGSIZE);
      clearbytes(usermem[i], USER_MEM_SIZE);
      p->pid = nextpid++;
      p->state = RUNNABLE;
      p->trapframe = (struct trapframe *)trapframe_pages[i];
      p->memory = usermem[i];
      p->pagetable = proc_pagetable(p);
      return p->pagetable ? p : 0;
    }
  }
  return 0;
}

static int
load(struct proc *p, const char *name)
{
  for (struct uprog *u = _uprog_table; u->start;
       u = (struct uprog *)((u->end + 7) & ~7ULL)) {
    if (streq(name, u->name)) {
      uint64 n = u->end - u->start;
      if (n > USER_MEM_SIZE - PGSIZE)
        return -1;
      clearbytes(p->memory, USER_MEM_SIZE);
      copybytes(p->memory, (void *)u->start, n);
      p->image_size = n;
      p->trapframe->epc = 0;
      p->trapframe->sp = USER_MEM_SIZE - 16;
      return 0;
    }
  }
  return -1;
}

void
procinit(void)
{
  clearbytes(procs, sizeof(procs));
}

void *
useraddr(struct proc *p, uint64 va, uint64 len)
{
  if (va >= USER_MEM_SIZE || len > USER_MEM_SIZE - va)
    return 0;
  return p->memory + va;
}

void
switch_to(struct proc *p)
{
  uint64 x;
  current_proc = p;
  p->state = RUNNING;
  p->trapframe->kernel_satp = proc_satp(p);
  p->trapframe->kernel_sp = (uint64)stack0_top;
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = 0;

  x = r_sstatus();
  x &= ~SSTATUS_SPP;
  x |= SSTATUS_SPIE;
  w_sstatus(x);
  w_sepc(p->trapframe->epc);
  w_stvec(TRAMPOLINE + ((uint64)uservec & (PGSIZE - 1)));
  ((void (*)(uint64))userret)(proc_satp(p));
  for (;;)
    ;
}

void
userinit(void)
{
  struct proc *p = allocproc();
  if (p == 0 || load(p, "sh") < 0)
    for (;;)
      asm volatile("wfi");
  switch_to(p);
}

int
proc_fork(void)
{
  struct proc *child = allocproc();
  if (child == 0)
    return -1;
  copybytes(child->memory, current_proc->memory, USER_MEM_SIZE);
  copybytes(child->trapframe, current_proc->trapframe, sizeof(*child->trapframe));
  child->trapframe->a0 = 0;
  child->parent = current_proc;
  child->image_size = current_proc->image_size;
  return child->pid;
}

int
proc_exec(const char *name)
{
  return load(current_proc, name);
}

int
proc_wait(uint64 status_addr)
{
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &procs[i];
    if (p->parent == current_proc && p->state == ZOMBIE) {
      int pid = p->pid;
      int *status = useraddr(current_proc, status_addr, sizeof(int));
      if (status)
        *status = p->exit_status;
      p->state = UNUSED;
      return pid;
    }
  }
  for (int i = 0; i < NPROC; i++) {
    if (procs[i].parent == current_proc && procs[i].state == RUNNABLE) {
      current_proc->state = WAITING;
      switch_to(&procs[i]);
    }
  }
  return -1;
}

void
proc_exit(int status)
{
  struct proc *p = current_proc;
  p->exit_status = status;
  p->state = ZOMBIE;
  if (p->parent && p->parent->state == WAITING) {
    p->parent->trapframe->a0 = p->pid;
    p->state = UNUSED;
    switch_to(p->parent);
  }
  for (;;)
    asm volatile("wfi");
}
