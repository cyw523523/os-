#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "buf.h"
#include "course_sid.h"
#include "defs.h"
#include "fsinternal.h"

#define OP_BLOCKS 6
#define PIN_LIMIT (LAB6_BUF_SLOTS - 3)

struct logheader {
  uint count;
  uint block[LAB6_LOG_BLOCKS];
};

static struct {
  struct spinlock lock;
  uint start, size, end;
  int active, committing;
  int crash_stage;
  struct logheader head;
  struct buf *pinned[LAB6_LOG_BLOCKS];
  uchar scratch[BSIZE];
} logstate = {.crash_stage = -1};

static void
checkpoint(int stage)
{
  if (__atomic_load_n(&logstate.crash_stage, __ATOMIC_ACQUIRE) == stage)
    crash_at(stage);
}

static void
write_head(void)
{
  struct buf *b = bread(logstate.start);
  memmove(b->data, &logstate.head, sizeof(logstate.head));
  bwrite(b);
  brelse(b);
}

// Use one cache buffer at a time, including when the cache is mostly pinned.
static void
transfer(uint from, uint to, struct buf *pin)
{
  struct buf *b = bread(from);
  memmove(logstate.scratch, b->data, BSIZE);
  brelse(b);
  b = bread(to);
  memmove(b->data, logstate.scratch, BSIZE);
  bwrite(b);
  if (pin) bunpin(pin);
  brelse(b);
}

static void
install_trans(int recovering)
{
  uint count = logstate.head.count;
  for (uint i = 0; i < count; i++) {
    transfer(logstate.start + 1 + i, logstate.head.block[i],
             recovering ? 0 : logstate.pinned[i]);
    if (!recovering && i + 1 == (count + 1) / 2)
      checkpoint(4);
  }
}

static void
recover_from_log(void)
{
  struct buf *b = bread(logstate.start);
  memmove(&logstate.head, b->data, sizeof(logstate.head));
  brelse(b);
  if (logstate.head.count > logstate.size)
    panic("invalid log length");
  if (logstate.head.count == 0) return;
  for (uint i = 0; i < logstate.head.count; i++)
    if (logstate.head.block[i] <= logstate.start + logstate.size ||
        logstate.head.block[i] >= logstate.end)
      panic("invalid log destination");
  printf("fs: replay %d blocks\n", (int)logstate.head.count);
  install_trans(1);
  logstate.head.count = 0;
  write_head();
}

void
loginit(const struct superblock *sb)
{
  logstate.start = sb->logstart;
  logstate.size = sb->nlog - 1;
  logstate.end = sb->size;
  recover_from_log();
}

int
log_arm_crash(int stage)
{
  if (stage < 0 || stage > 7) return -1;
  __atomic_store_n(&logstate.crash_stage, stage, __ATOMIC_RELEASE);
  return 0;
}

void
begin_op(void)
{
  acquire(&logstate.lock);
  while (logstate.committing ||
         logstate.head.count + (logstate.active + 1) * OP_BLOCKS > logstate.size ||
         logstate.head.count + (logstate.active + 1) * OP_BLOCKS > PIN_LIMIT)
    sleep(&logstate, &logstate.lock);
  checkpoint(0);
  logstate.active++;
  release_lock(&logstate.lock);
  checkpoint(1);
}

void
log_write(struct buf *b)
{
  acquire(&logstate.lock);
  if (!logstate.active) panic("log_write outside transaction");
  uint i;
  for (i = 0; i < logstate.head.count; i++)
    if (logstate.head.block[i] == b->blockno) break;
  if (i == logstate.head.count) {
    if (i >= logstate.size || i >= PIN_LIMIT) panic("log_write capacity");
    bpin(b);
    logstate.pinned[i] = b;
    logstate.head.block[i] = b->blockno;
    logstate.head.count++;
  }
  release_lock(&logstate.lock);
}

static void
commit(void)
{
  uint count = logstate.head.count;
  if (!count) return;
  for (uint i = 0; i < count; i++) {
    transfer(logstate.head.block[i], logstate.start + 1 + i, 0);
    if (i + 1 == (count + 1) / 2) checkpoint(2);
  }
  checkpoint(6);
  write_head();
  checkpoint(3);
  install_trans(0);
  checkpoint(7);
  logstate.head.count = 0;
  write_head();
  checkpoint(5);
}

void
end_op(void)
{
  acquire(&logstate.lock);
  if (--logstate.active != 0) {
    wakeup(&logstate);
    release_lock(&logstate.lock);
    return;
  }
  logstate.committing = 1;
  release_lock(&logstate.lock);
  commit();
  acquire(&logstate.lock);
  logstate.committing = 0;
  wakeup(&logstate);
  release_lock(&logstate.lock);
}

void
log_sync(void)
{
  acquire(&logstate.lock);
  while (logstate.active || logstate.committing)
    sleep(&logstate, &logstate.lock);
  release_lock(&logstate.lock);
}
