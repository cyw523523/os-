#include "types.h"
#include "riscv.h"
#include "spinlock.h"

// Lab4's required baseline is one hart. Disabling local interrupts makes
// lock transitions atomic with traps and the scheduler on that hart.
static int noff;
static int intena;

void
acquire(struct spinlock *lock)
{
  int enabled = intr_get();
  intr_off();
  if (noff++ == 0)
    intena = enabled;
  if (lock->locked)
    for (;;) asm volatile("wfi");
  lock->locked = 1;
}

void
release_lock(struct spinlock *lock)
{
  if (!lock->locked)
    for (;;) asm volatile("wfi");
  lock->locked = 0;
  if (--noff == 0 && intena)
    intr_on();
}

int
lock_depth(void)
{
  return noff;
}
