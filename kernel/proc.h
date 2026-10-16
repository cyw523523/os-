#ifndef PROC_H
#define PROC_H

#define NPROC 4
#define USER_MEM_SIZE KERNBASE
#define USER_STACK_BASE (USER_MEM_SIZE - 4 * PGSIZE)

struct trapframe {
  uint64 kernel_satp, kernel_sp, kernel_trap, epc, kernel_hartid;
  uint64 ra, sp, gp, tp, t0, t1, t2, s0, s1;
  uint64 a0, a1, a2, a3, a4, a5, a6, a7;
  uint64 s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
  uint64 t3, t4, t5, t6;
};

enum procstate { UNUSED, RUNNABLE, RUNNING, WAITING, ZOMBIE };

struct proc {
  int pid;
  enum procstate state;
  struct proc *parent;
  int exit_status;
  uint64 image_size;
  uint64 size;
  pagetable_t pagetable;
  struct trapframe *trapframe;
};

extern struct proc *current_proc;

void procinit(void);
void userinit(void) __attribute__((noreturn));
void switch_to(struct proc *p) __attribute__((noreturn));
int proc_fork(void);
int proc_exec(const char *name);
int proc_wait(uint64 status_addr);
void proc_exit(int status) __attribute__((noreturn));
pagetable_t proc_pagetable(struct proc *p);
void proc_freepagetable(pagetable_t root);
uint64 proc_satp(struct proc *p);
void kinit(void);
void kvminit(void);
uint64 kernel_satp(void);
void *kalloc(void);
void kfree(void *address);
uint64 kpages_used(void);
int uvmalloc(pagetable_t root, uint64 start, uint64 end, int flags);
void uvmdealloc(pagetable_t root, uint64 start, uint64 end);
int uvmcopy(pagetable_t source, pagetable_t target, uint64 start, uint64 end);
int usercopy(pagetable_t root, uint64 va, void *buffer, uint64 count, int to_user);
int usercheck(pagetable_t root, uint64 va, uint64 count, int write);
void dump_pagetable(pagetable_t root);
uint64 proc_sbrk(int amount);

#endif
