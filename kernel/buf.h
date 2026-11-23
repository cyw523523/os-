#ifndef BUF_H
#define BUF_H

#include "fs.h"
#include "sleeplock.h"

struct buf {
  uint blockno;
  int disk;
  int valid;
  int assigned;
  uint refs, frequency;
  uint64 last_use;
  int dirty;
  struct sleeplock lock;
  uchar data[BSIZE] __attribute__((aligned(8)));
};

void virtio_disk_init(void);
void virtio_disk_intr(void);
void virtio_disk_rw(struct buf *, int);
struct buf *bread(uint);
void bwrite(struct buf *);
void brelse(struct buf *);
void bpin(struct buf *);
void bunpin(struct buf *);
void bstats(void);

#endif
