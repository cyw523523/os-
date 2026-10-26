#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "proc.h"

struct uprog { uint64 start, end; char name[8]; };
extern struct uprog _uprog_table[];
void usertrapret(void) __attribute__((noreturn));
void swtch(struct context *, struct context *);

static struct proc procs[NPROC];
struct cpu cpus[LAB4_NCPU];
static struct proc *initproc;
static struct spinlock wait_lock, ticks_lock;
static uint64 ticks;
static int nextpid = 1;

struct proc *myproc(void) { return cpus[r_tp()].proc; }

static void
copy_name(char dst[8], const char *src)
{
  int i = 0;
  for (; i < 7 && src[i] && src[i] != '\n' && src[i] != '\r'; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static int
streq(const char *left, const char *right)
{
  while (*left && *left == *right) left++, right++;
  return (*left == 0 || *left == '\n' || *left == '\r') && *right == 0;
}

// Only wait() reaps a zombie; exit must keep its own kernel stack alive.
static void
freeproc(struct proc *p)
{
  if (p->pagetable) proc_freepagetable(p->pagetable);
  if (p->trapframe) kfree(p->trapframe);
  if (p->kstack) kfree(p->kstack);
  p->pagetable = 0;
  p->trapframe = 0;
  p->kstack = 0;
  p->parent = 0;
  p->chan = 0;
  p->pid = 0;
  p->state = UNUSED;
}

static void
forkret(void)
{
  release_lock(&current_proc->lock);
  usertrapret();
}

static struct proc *
allocproc(void)
{
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &procs[i];
    acquire(&p->lock);
    if (p->state != UNUSED) {
      release_lock(&p->lock);
      continue;
    }
    p->state = USED;
    p->pid = __sync_fetch_and_add(&nextpid, 1);
    p->parent = 0;
    p->size = p->image_size = 0;
    p->killed = p->exit_status = 0;
    p->chan = 0;
    p->tickets = 7 + COURSE_SID % 13;
    p->cpu_ticks = p->last_slice_ticks = 0;
    p->name[0] = 0;
    for (int fd = 0; fd < 16; fd++) {
      p->pipes[fd] = 0;
      p->fdmode[fd] = 0;
    }
    p->pagetable = 0;
    p->trapframe = kalloc();
    p->kstack = kalloc();
    if (p->trapframe && p->kstack) p->pagetable = proc_pagetable(p);
    if (p->pagetable) {
      p->context.ra = (uint64)forkret;
      p->context.sp = (uint64)p->kstack + PGSIZE;
      release_lock(&p->lock);
      return p;
    }
    freeproc(p);
    release_lock(&p->lock);
    return 0;
  }
  return 0;
}

static int
load(struct proc *p, const char *name)
{
  for (struct uprog *program = _uprog_table; program->start;
       program = (struct uprog *)((program->end + 7) & ~7ULL)) {
    if (!streq(name, program->name)) continue;
    uint64 image_size = program->end - program->start;
    uint64 image_end = PGROUNDUP(PGSIZE + image_size);
    uint64 guard_start = USER_STACK_BASE - LAB3_GUARD_PAGES * PGSIZE;
    if (image_end >= guard_start) return -1;
    pagetable_t replacement = proc_pagetable(p);
    if (!replacement) return -1;
    if (uvmalloc(replacement, PGSIZE, image_end, PTE_R | PTE_W | PTE_X) < 0 ||
        uvmalloc(replacement, USER_STACK_BASE, USER_STACK_BASE + PGSIZE,
                 PTE_R | PTE_W) < 0 ||
        usercopy(replacement, PGSIZE, (void *)program->start, image_size, 1) < 0) {
      proc_freepagetable(replacement);
      return -1;
    }
    pagetable_t old = p->pagetable;
    p->pagetable = replacement;
    p->image_size = image_size;
    p->size = image_end;
    p->trapframe->epc = PGSIZE;
    p->trapframe->sp = USER_STACK_BASE + PGSIZE - 16;
    copy_name(p->name, program->name);
    if (LAB4_POLICY_ID == 5) {
      if (streq(p->name, "hi")) p->tickets = 1;
      else if (streq(p->name, "spin")) p->tickets = 2;
      else if (streq(p->name, "sh")) p->tickets = 4;
    }
    proc_freepagetable(old);
    return 0;
  }
  return -1;
}

void
procinit(void)
{
  for (int i = 0; i < NPROC; i++) procs[i].state = UNUSED;
  for (int i = 0; i < LAB4_NCPU; i++)
    cpus[i].random_state = COURSE_SID ^ (0x9e3779b97f4a7c15ULL * (i + 1));
}

void
userinit(void)
{
  struct proc *p = allocproc();
  if (!p || load(p, "sh") < 0) for (;;) asm volatile("wfi");
  initproc = p;
  dump_pagetable(p->pagetable);
  acquire(&p->lock);
  p->state = RUNNABLE;
  release_lock(&p->lock);
}

static void
sched(void)
{
  if (!current_proc || lock_depth() != 1 || intr_get() ||
      current_proc->state == RUNNING)
    for (;;) asm volatile("wfi");
  swtch(&current_proc->context, &cpus[r_tp()].scheduler);
}

static uint64
random64(void)
{
  struct cpu *c = &cpus[r_tp()];
  c->random_state ^= c->random_state << 13;
  c->random_state ^= c->random_state >> 7;
  c->random_state ^= c->random_state << 17;
  return c->random_state * 0x2545F4914F6CDD1DULL;
}

void
scheduler(void)
{
  for (;;) {
    intr_on();
    // Each candidate is locked again before RUNNING is set; two harts cannot
    // dispatch the same process even if the runnable set changes mid-scan.
    intr_off();
    int max_tickets = 0, leader_ticks = 0;
    for (int i = 0; i < NPROC; i++) {
      acquire(&procs[i].lock);
      if (procs[i].state == RUNNABLE || procs[i].state == RUNNING) {
        if (procs[i].tickets > max_tickets) {
          max_tickets = procs[i].tickets;
          leader_ticks = procs[i].cpu_ticks;
        } else if (procs[i].tickets == max_tickets &&
                   procs[i].cpu_ticks > leader_ticks)
          leader_ticks = procs[i].cpu_ticks;
      }
      release_lock(&procs[i].lock);
    }
    int total = 0;
    for (int i = 0; i < NPROC; i++) {
      struct proc *p = &procs[i];
      acquire(&p->lock);
      if (p->state == RUNNABLE &&
          (uint64)p->cpu_ticks * max_tickets <=
            (uint64)leader_ticks * p->tickets + 2ULL * max_tickets)
        total += p->tickets;
      release_lock(&p->lock);
    }
    if (!total) {
      intr_on();
      asm volatile("wfi");
      continue;
    }
    int draw = random64() % total;
    for (int i = 0; i < NPROC; i++) {
      struct proc *p = &procs[i];
      acquire(&p->lock);
      if (p->state == RUNNABLE &&
          (uint64)p->cpu_ticks * max_tickets <=
            (uint64)leader_ticks * p->tickets + 2ULL * max_tickets) {
        if (draw < p->tickets) {
          p->state = RUNNING;
          p->last_slice_ticks = 0;
          cpus[r_tp()].proc = p;
          swtch(&cpus[r_tp()].scheduler, &p->context);
          cpus[r_tp()].proc = 0;
          release_lock(&p->lock);
          break;
        }
        draw -= p->tickets;
      }
      release_lock(&p->lock);
    }
  }
}

void
yield(void)
{
  struct proc *p = current_proc;
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release_lock(&p->lock);
}

void
sleep(void *chan, struct spinlock *lock)
{
  struct proc *p = current_proc;
  acquire(&p->lock);
  release_lock(lock);
  p->chan = chan;
  p->state = SLEEPING;
  sched();
  p->chan = 0;
  release_lock(&p->lock);
  acquire(lock);
}

void
wakeup(void *chan)
{
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &procs[i];
    if (p == current_proc) continue;
    acquire(&p->lock);
    if (p->state == SLEEPING && p->chan == chan) p->state = RUNNABLE;
    release_lock(&p->lock);
  }
}

void
proc_tick(void)
{
  if (r_tp() == 0) {
    acquire(&ticks_lock);
    ticks++;
    wakeup(&ticks);
    release_lock(&ticks_lock);
  }
  struct proc *p = current_proc;
  if (p) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->cpu_ticks++;
      p->last_slice_ticks++;
    }
    release_lock(&p->lock);
  }
}

uint64
proc_ticks(void)
{
  acquire(&ticks_lock);
  uint64 result = ticks;
  release_lock(&ticks_lock);
  return result;
}

void
proc_pause(int count)
{
  if (count <= 0) return;
  acquire(&ticks_lock);
  uint64 until = ticks + count;
  while (ticks < until && !current_proc->killed) sleep(&ticks, &ticks_lock);
  release_lock(&ticks_lock);
}

int
proc_fork(void)
{
  struct proc *child = allocproc();
  if (!child) return -1;
  struct proc *parent = current_proc;
  if (uvmcopy(parent->pagetable, child->pagetable, 0, parent->size) < 0 ||
      uvmcopy(parent->pagetable, child->pagetable,
              USER_STACK_BASE, USER_STACK_BASE + PGSIZE) < 0) {
    acquire(&child->lock);
    freeproc(child);
    release_lock(&child->lock);
    return -1;
  }
  uchar *source = (uchar *)parent->trapframe;
  uchar *target = (uchar *)child->trapframe;
  for (uint64 i = 0; i < sizeof(*child->trapframe); i++) target[i] = source[i];
  child->trapframe->a0 = 0;
  child->image_size = parent->image_size;
  child->size = parent->size;
  child->tickets = parent->tickets;
  copy_name(child->name, parent->name);
  pipefork(parent, child);
  acquire(&wait_lock);
  child->parent = parent;
  acquire(&child->lock);
  child->state = RUNNABLE;
  release_lock(&child->lock);
  release_lock(&wait_lock);
  return child->pid;
}

int proc_exec(const char *name) { return load(current_proc, name); }

uint64
proc_sbrk(int amount)
{
  struct proc *p = current_proc;
  uint64 old = p->size;
  uint64 limit = USER_STACK_BASE - LAB3_GUARD_PAGES * PGSIZE;
  if (amount < 0) {
    uint64 decrease = -(long)amount;
    uint64 minimum = PGROUNDUP(PGSIZE + p->image_size);
    if (decrease > old - minimum) return -1;
    p->size = old - decrease;
    uvmdealloc(p->pagetable, p->size, old);
    sfence_vma();
    return old;
  }
  if ((uint64)amount > limit - old) return -1;
  uint64 next = old + amount;
  if (uvmalloc(p->pagetable, old, next, PTE_R | PTE_W) < 0) return -1;
  p->size = next;
  return old;
}

int
proc_wait(uint64 status_addr)
{
  struct proc *parent = current_proc;
  acquire(&wait_lock);
  for (;;) {
    int have_child = 0;
    for (int i = 0; i < NPROC; i++) {
      struct proc *p = &procs[i];
      if (p->parent != parent) continue;
      have_child = 1;
      acquire(&p->lock);
      if (p->state == ZOMBIE) {
        int pid = p->pid, status = p->exit_status;
        if (status_addr && usercopy(parent->pagetable, status_addr,
                                    &status, sizeof(status), 1) < 0) {
          release_lock(&p->lock);
          release_lock(&wait_lock);
          return -1;
        }
        freeproc(p);
        release_lock(&p->lock);
        release_lock(&wait_lock);
        return pid;
      }
      release_lock(&p->lock);
    }
    if (!have_child || parent->killed) {
      release_lock(&wait_lock);
      return -1;
    }
    sleep(parent, &wait_lock);
  }
}

void
proc_exit(int status)
{
  struct proc *p = current_proc;
  pipeexit(p);
  acquire(&wait_lock);
  for (int i = 0; i < NPROC; i++) {
    if (procs[i].parent == p) {
      procs[i].parent = initproc;
      wakeup(initproc);
    }
  }
  if (p->parent) wakeup(p->parent);
  acquire(&p->lock);
  p->exit_status = status;
  p->state = ZOMBIE;
  release_lock(&wait_lock);
  sched();
  for (;;) asm volatile("wfi");
}

int
proc_kill(int pid)
{
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &procs[i];
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED && p->state != ZOMBIE) {
      p->killed = 1;
      if (p->state == SLEEPING) p->state = RUNNABLE;
      release_lock(&p->lock);
      return 0;
    }
    release_lock(&p->lock);
  }
  return -1;
}

int
proc_schedstat(int pid, uint64 cpu_addr, uint64 slice_addr)
{
  if (!cpu_addr || !slice_addr) return -1;
  struct proc *requester = current_proc;
  if (pid == -1) pid = requester->pid;
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &procs[i];
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED && p->state != ZOMBIE) {
      int cpu = p->cpu_ticks, slice = p->last_slice_ticks;
      release_lock(&p->lock);
      if (usercopy(requester->pagetable, cpu_addr, &cpu, sizeof(cpu), 1) < 0 ||
          usercopy(requester->pagetable, slice_addr, &slice, sizeof(slice), 1) < 0)
        return -1;
      return 0;
    }
    release_lock(&p->lock);
  }
  return -1;
}

int
proc_setickets(int tickets)
{
  if (tickets < 1 || tickets > 1000) return -1;
  acquire(&current_proc->lock);
  current_proc->tickets = tickets;
  release_lock(&current_proc->lock);
  return 0;
}
