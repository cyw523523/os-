#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"

void
initlock(struct spinlock *lock, const char *name)
{
  (void)name;
  lock->locked = 0;
  lock->owner = -1;
}

void
acquiresleep(struct sleeplock *lock)
{
  acquire(&lock->guard);
  while (lock->locked)
    sleep(lock, &lock->guard);
  lock->locked = 1;
  release_lock(&lock->guard);
}

void
releasesleep(struct sleeplock *lock)
{
  acquire(&lock->guard);
  lock->locked = 0;
  wakeup(lock);
  release_lock(&lock->guard);
}

void *
memset(void *dst, int value, unsigned long n)
{
  uchar *p = dst;
  while (n--) *p++ = value;
  return dst;
}

void *
memmove(void *dst, const void *src, unsigned long n)
{
  uchar *d = dst;
  const uchar *s = src;
  if ((uint64)d > (uint64)s && (uint64)d - (uint64)s < n) {
    while (n) {
      n--;
      d[n] = s[n];
    }
  } else {
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
  }
  return dst;
}

void *
memcpy(void *dst, const void *src, unsigned long n)
{
  return memmove(dst, src, n);
}

void
panic(const char *message)
{
  intr_off();
  printf("panic: %s\n", message);
  for (;;) asm volatile("wfi");
}
