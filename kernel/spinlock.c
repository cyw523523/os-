#include "types.h"
#include "riscv.h"
#include "proc.h"

void
acquire(struct spinlock *lock)
{
  struct cpu *c = &cpus[r_tp()];
  int enabled = intr_get();
  intr_off();
  if (c->noff++ == 0) c->intena = enabled;
  if (lock->locked && lock->owner == (int)r_tp())
    for (;;) asm volatile("wfi");
  while (__sync_lock_test_and_set(&lock->locked, 1))
    ;
  __sync_synchronize();
  lock->owner = r_tp();
}

void
release_lock(struct spinlock *lock)
{
  struct cpu *c = &cpus[r_tp()];
  if (!lock->locked || lock->owner != (int)r_tp() || c->noff < 1)
    for (;;) asm volatile("wfi");
  lock->owner = -1;
  __sync_synchronize();
  __sync_lock_release(&lock->locked);
  if (--c->noff == 0 && c->intena) intr_on();
}

int lock_depth(void) { return cpus[r_tp()].noff; }
