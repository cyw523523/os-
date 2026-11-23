#ifndef SLEEPLOCK_H
#define SLEEPLOCK_H

#include "spinlock.h"

struct sleeplock {
  struct spinlock guard;
  int locked;
};

void acquiresleep(struct sleeplock *);
void releasesleep(struct sleeplock *);

#endif
