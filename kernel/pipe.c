#include "types.h"
#include "riscv.h"
#include "proc.h"

#define NPIPE 8
#define PIPESIZE 512
struct pipe {
  struct spinlock lock;
  int used;
  int readers, writers;
  uint64 nread, nwrite;
  uchar data[PIPESIZE];
};

static struct pipe pipes[NPIPE];
static struct spinlock table_lock;

int
pipealloc(uint64 user_fds)
{
  struct proc *p = current_proc;
  int fd0 = -1, fd1 = -1;
  for (int i = 3; i < 16; i++) {
    if (!p->fdmode[i] && !p->files[i]) {
      if (fd0 < 0) fd0 = i;
      else { fd1 = i; break; }
    }
  }
  if (fd1 < 0) return -1;
  acquire(&table_lock);
  for (int i = 0; i < NPIPE; i++) {
    struct pipe *q = &pipes[i];
    if (q->used) continue;
    int result[2] = {fd0, fd1};
    if (usercopy(p->pagetable, user_fds, result, sizeof(result), 1) < 0) {
      release_lock(&table_lock);
      return -1;
    }
    q->used = 1;
    q->readers = q->writers = 1;
    q->nread = q->nwrite = 0;
    p->pipes[fd0] = p->pipes[fd1] = q;
    p->fdmode[fd0] = 1;
    p->fdmode[fd1] = 2;
    release_lock(&table_lock);
    return 0;
  }
  release_lock(&table_lock);
  return -1;
}

void
pipefork(struct proc *parent, struct proc *child)
{
  for (int i = 3; i < 16; i++) {
    struct pipe *q = parent->pipes[i];
    if (!q) continue;
    acquire(&q->lock);
    child->pipes[i] = q;
    child->fdmode[i] = parent->fdmode[i];
    if (child->fdmode[i] == 1) q->readers++;
    else q->writers++;
    release_lock(&q->lock);
  }
}

int
pipeclose(int fd)
{
  if (fd < 3 || fd >= 16 || !current_proc->pipes[fd]) return -1;
  struct pipe *q = current_proc->pipes[fd];
  acquire(&q->lock);
  if (current_proc->fdmode[fd] == 1) q->readers--;
  else q->writers--;
  current_proc->pipes[fd] = 0;
  current_proc->fdmode[fd] = 0;
  wakeup(&q->nread);
  wakeup(&q->nwrite);
  if (!q->readers && !q->writers) {
    acquire(&table_lock);
    q->used = 0;
    release_lock(&table_lock);
  }
  release_lock(&q->lock);
  return 0;
}

void
pipeexit(struct proc *p)
{
  (void)p;
  for (int i = 3; i < 16; i++)
    if (current_proc->pipes[i]) pipeclose(i);
}

int
piperead(int fd, uint64 address, int count)
{
  struct proc *p = current_proc;
  if (fd < 3 || fd >= 16 || p->fdmode[fd] != 1 ||
      count < 0 || count > 65536 ||
      usercheck(p->pagetable, address, count, 1) < 0) return -1;
  struct pipe *q = p->pipes[fd];
  acquire(&q->lock);
  int i = 0;
  while (i < count) {
    while (q->nread == q->nwrite && q->writers && !p->killed)
      sleep(&q->nread, &q->lock);
    if (p->killed) { release_lock(&q->lock); return -1; }
    if (q->nread == q->nwrite) break;
    char c = q->data[q->nread++ % PIPESIZE];
    if (usercopy(p->pagetable, address + i, &c, 1, 1) < 0) {
      release_lock(&q->lock);
      return -1;
    }
    i++;
    wakeup(&q->nwrite);
  }
  release_lock(&q->lock);
  return i;
}

int
pipewrite(int fd, uint64 address, int count)
{
  struct proc *p = current_proc;
  if (fd < 3 || fd >= 16 || p->fdmode[fd] != 2 ||
      count < 0 || count > 65536 ||
      usercheck(p->pagetable, address, count, 0) < 0) return -1;
  struct pipe *q = p->pipes[fd];
  acquire(&q->lock);
  int i = 0;
  while (i < count) {
    while (q->nwrite - q->nread == PIPESIZE && q->readers && !p->killed)
      sleep(&q->nwrite, &q->lock);
    if (!q->readers || p->killed) {
      release_lock(&q->lock);
      return -1;
    }
    char c;
    if (usercopy(p->pagetable, address + i, &c, 1, 0) < 0) {
      release_lock(&q->lock);
      return -1;
    }
    q->data[q->nwrite++ % PIPESIZE] = c;
    i++;
    wakeup(&q->nread);
  }
  release_lock(&q->lock);
  return i;
}
