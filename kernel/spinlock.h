#ifndef SPINLOCK_H
#define SPINLOCK_H

struct spinlock { int locked; };
void acquire(struct spinlock *lock);
void release_lock(struct spinlock *lock);
int lock_depth(void);

#endif
