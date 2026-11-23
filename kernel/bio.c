#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "course_sid.h"
#include "buf.h"
#include "defs.h"

#if LAB6_REPLACE_POLICY != 3
#error "Lab6 requires the assigned LFU policy"
#endif

static struct {
  struct spinlock lock;
  struct buf slots[LAB6_BUF_SLOTS];
  uint64 accesses, hits, evictions, writes, dirty_writes;
} cache;

struct buf *
bread(uint blockno)
{
  struct buf *b;
  acquire(&cache.lock);
  for (;;) {
    struct buf *victim = 0;
    for (b = cache.slots; b < cache.slots + LAB6_BUF_SLOTS; b++) {
      if (b->assigned && b->blockno == blockno) break;
      if (b->refs) continue;
      if (!victim || !b->assigned ||
          (victim->assigned && (b->frequency < victim->frequency ||
           (b->frequency == victim->frequency && b->last_use < victim->last_use))))
        victim = b;
    }
    if (b < cache.slots + LAB6_BUF_SLOTS) {
      cache.hits++;
      break;
    }
    if (victim) {
      b = victim;
      if (b->dirty) panic("evict dirty block");
      if (b->assigned) cache.evictions++;
      b->assigned = 1;
      b->blockno = blockno;
      b->valid = 0;
      b->frequency = 0;
      break;
    }
    sleep(&cache, &cache.lock);
  }
  b->refs++;
  if (++cache.accesses % (4 * LAB6_BUF_SLOTS) == 0)
    for (int i = 0; i < LAB6_BUF_SLOTS; i++)
      cache.slots[i].frequency /= 2;
  b->frequency++;
  b->last_use = cache.accesses;
  release_lock(&cache.lock);

  acquiresleep(&b->lock);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

void
bwrite(struct buf *b)
{
  virtio_disk_rw(b, 1);
  acquire(&cache.lock);
  cache.writes++;
  if (b->dirty) cache.dirty_writes++;
  b->dirty = 0;
  release_lock(&cache.lock);
}

void
bpin(struct buf *b)
{
  acquire(&cache.lock);
  b->refs++;
  b->dirty = 1;
  release_lock(&cache.lock);
}

void
bunpin(struct buf *b)
{
  acquire(&cache.lock);
  if (b->dirty) panic("unpin dirty block");
  if (--b->refs == 0) wakeup(&cache);
  release_lock(&cache.lock);
}

void
brelse(struct buf *b)
{
  releasesleep(&b->lock);
  acquire(&cache.lock);
  if (--b->refs == 0) wakeup(&cache);
  release_lock(&cache.lock);
}

void
bstats(void)
{
  uint64 hits, evictions, writes, dirty_writes;
  acquire(&cache.lock);
  for (int i = 0; i < LAB6_BUF_SLOTS; i++)
    for (int j = i + 1; j < LAB6_BUF_SLOTS; j++)
      if (cache.slots[i].assigned && cache.slots[j].assigned &&
          cache.slots[i].blockno == cache.slots[j].blockno)
        panic("duplicate cached block");
  hits = cache.hits;
  evictions = cache.evictions;
  writes = cache.writes;
  dirty_writes = cache.dirty_writes;
  release_lock(&cache.lock);
  printf("cache hits=%d evictions=%d writes=%d dirty_writebacks=%d\n",
         (int)hits, (int)evictions, (int)writes, (int)dirty_writes);
}
