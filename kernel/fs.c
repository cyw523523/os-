#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "param.h"
#include "course_sid.h"
#include "buf.h"
#include "defs.h"
#include "fsinternal.h"
#include "stat.h"

static struct superblock sb;
static struct {
  struct spinlock lock;
  struct inode entries[NINODE];
} icache;

void
fsinit(void)
{
  struct buf *b = bread(1);
  memmove(&sb, b->data, sizeof(sb));
  brelse(b);
  if (sb.magic != FSMAGIC || sb.size != FSSIZE ||
      sb.nlog != LAB6_LOG_BLOCKS + 1 || sb.logstart != 2 ||
      sb.inodestart != sb.logstart + sb.nlog || sb.ninodes < 2 ||
      sb.ninodes >= 65536 ||
      sb.bmapstart != sb.inodestart + sb.ninodes / IPB + 1 ||
      sb.bmapstart >= sb.size || sb.nblocks != sb.size - sb.bmapstart - 1)
    panic("invalid superblock");
  loginit(&sb);

  // A reset discards every open descriptor. Reap durable, unlinked inodes.
  for (uint inum = ROOTINO + 1; inum < sb.ninodes; inum++) {
    b = bread(IBLOCK(inum, sb));
    struct dinode *d = (struct dinode *)b->data + inum % IPB;
    int orphan = d->type && d->nlink == 0;
    brelse(b);
    if (orphan) {
      begin_op();
      struct inode *ip = iget(inum);
      ilock(ip);
      iunlock(ip);
      iput(ip);
      end_op();
    }
  }
}

static uint
balloc(void)
{
  // The course image has one bitmap block (FSSIZE < BPB).
  struct buf *b = bread(sb.bmapstart);
  for (uint block = sb.size - sb.nblocks; block < sb.size; block++) {
    uchar mask = 1 << (block % 8);
    if (b->data[block / 8] & mask) continue;
    b->data[block / 8] |= mask;
    log_write(b);
    brelse(b);
    b = bread(block);
    memset(b->data, 0, BSIZE);
    log_write(b);
    brelse(b);
    return block;
  }
  brelse(b);
  return 0;
}

static void
bfree(uint block)
{
  struct buf *b = bread(BBLOCK(block, sb));
  uchar mask = 1 << (block % 8);
  if (!(b->data[block / 8] & mask)) panic("free block twice");
  b->data[block / 8] &= ~mask;
  log_write(b);
  brelse(b);
}

static struct inode *
iget_mode(uint inum, int fresh)
{
  if (inum == 0 || inum >= sb.ninodes) panic("bad inode number");
  acquire(&icache.lock);
  struct inode *free = 0;
  for (int i = 0; i < NINODE; i++) {
    struct inode *ip = &icache.entries[i];
    if (ip->refs && ip->inum == inum) {
      if (!fresh) ip->refs++;
      release_lock(&icache.lock);
      return fresh ? 0 : ip;
    }
    if (!ip->refs && !free) free = ip;
  }
  if (!free) {
    release_lock(&icache.lock);
    return 0;
  }
  free->inum = inum;
  free->refs = 1;
  free->valid = 0;
  release_lock(&icache.lock);
  return free;
}

struct inode *
iget(uint inum)
{
  return iget_mode(inum, 0);
}

int
inode_unreferenced(struct inode *ip)
{
  acquire(&icache.lock);
  int sole = ip->refs == 1;
  release_lock(&icache.lock);
  return sole;
}

struct inode *
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->refs++;
  release_lock(&icache.lock);
  return ip;
}

void
ilock(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if (!ip->valid) {
    struct buf *b = bread(IBLOCK(ip->inum, sb));
    ip->disk = ((struct dinode *)b->data)[ip->inum % IPB];
    brelse(b);
    ip->valid = 1;
    if (!ip->disk.type) panic("unallocated inode");
  }
}

void
iunlock(struct inode *ip)
{
  releasesleep(&ip->lock);
}

void
iupdate(struct inode *ip)
{
  struct buf *b = bread(IBLOCK(ip->inum, sb));
  ((struct dinode *)b->data)[ip->inum % IPB] = ip->disk;
  log_write(b);
  brelse(b);
}

struct inode *
ialloc(short type)
{
  for (uint inum = ROOTINO + 1; inum < sb.ninodes; inum++) {
    struct buf *b = bread(IBLOCK(inum, sb));
    struct dinode *d = (struct dinode *)b->data + inum % IPB;
    if (!d->type) {
      struct inode *ip = iget_mode(inum, 1);
      if (ip) {
        memset(d, 0, sizeof(*d));
        d->type = type;
        log_write(b);
      }
      brelse(b);
      if (ip) return ip;
      continue;
    }
    brelse(b);
  }
  return 0;
}

void
itrunc(struct inode *ip)
{
  for (uint i = 0; i < NDIRECT; i++)
    if (ip->disk.addrs[i]) bfree(ip->disk.addrs[i]);

  uint indirect = ip->disk.addrs[NDIRECT];
  if (indirect) {
    uint blocks[NINDIRECT];
    struct buf *b = bread(indirect);
    memmove(blocks, b->data, sizeof(blocks));
    brelse(b);
    for (uint i = 0; i < NINDIRECT; i++)
      if (blocks[i]) bfree(blocks[i]);
    bfree(indirect);
  }
  memset(ip->disk.addrs, 0, sizeof(ip->disk.addrs));
  ip->disk.size = 0;
  iupdate(ip);
}

void
iput(struct inode *ip)
{
  if (!ip) return;
  acquire(&icache.lock);
  if (ip->refs == 1 && ip->valid && ip->disk.nlink == 0) {
    // The final reference owns the inode. No other holder can take its lock.
    ilock(ip);
    release_lock(&icache.lock);
    itrunc(ip);
    ip->disk.type = 0;
    iupdate(ip);
    ip->valid = 0;
    iunlock(ip);
    acquire(&icache.lock);
  }
  ip->refs--;
  release_lock(&icache.lock);
}

static uint
bmap(struct inode *ip, uint index, int allocate)
{
  if (index < NDIRECT) {
    uint *address = &ip->disk.addrs[index];
    if (!*address && allocate) *address = balloc();
    return *address;
  }
  if (index >= MAXFILE) return 0;
  uint *indirect = &ip->disk.addrs[NDIRECT];
  if (!*indirect && allocate) *indirect = balloc();
  if (!*indirect) return 0;

  index -= NDIRECT;
  struct buf *b = bread(*indirect);
  uint address = ((uint *)b->data)[index];
  brelse(b);
  if (!address && allocate) {
    address = balloc();
    if (address) {
      b = bread(*indirect);
      ((uint *)b->data)[index] = address;
      log_write(b);
      brelse(b);
    }
  }
  return address;
}

int
iread(struct inode *ip, int user, uint64 dst, uint off, uint n)
{
  if (off >= ip->disk.size) return 0;
  if (n > ip->disk.size - off) n = ip->disk.size - off;
  uint done = 0;
  while (done < n) {
    uint address = bmap(ip, off / BSIZE, 0);
    if (!address) return -1;
    struct buf *b = bread(address);
    uint count = BSIZE - off % BSIZE;
    if (count > n - done) count = n - done;
    uchar *src = b->data + off % BSIZE;
    int error = 0;
    if (user)
      error = usercopy(current_proc->pagetable, dst, src, count, 1);
    else
      memmove((void *)dst, src, count);
    brelse(b);
    if (error < 0) return done ? (int)done : -1;
    off += count;
    dst += count;
    done += count;
  }
  return done;
}

// The caller bounds each write operation to one data block.
int
iwrite(struct inode *ip, int user, uint64 src, uint off, uint n)
{
  if (!n) return 0;
  if (off > ip->disk.size || off >= MAXFILE * BSIZE || n > BSIZE - off % BSIZE)
    return -1;
  uchar data[BSIZE];
  if (user) {
    if (usercopy(current_proc->pagetable, src, data, n, 0) < 0) return -1;
  } else {
    memmove(data, (void *)src, n);
  }
  uint address = bmap(ip, off / BSIZE, 1);
  if (address) {
    struct buf *b = bread(address);
    memmove(b->data + off % BSIZE, data, n);
    log_write(b);
    brelse(b);
    if (off + n > ip->disk.size) ip->disk.size = off + n;
  }
  // Persist a newly allocated indirect block even if the data allocation failed.
  iupdate(ip);
  return address ? (int)n : -1;
}

static uint
dirfind(struct inode *dir, const char *name, uint *offset, uint *available)
{
  if (dir->disk.type != T_DIR) return 0;
  struct dirent ent;
  if (available) *available = dir->disk.size;
  for (uint off = 0; off < dir->disk.size; off += sizeof(ent)) {
    if (iread(dir, 0, (uint64)&ent, off, sizeof(ent)) != sizeof(ent))
      panic("invalid directory block");
    if (!ent.inum) {
      if (available && *available == dir->disk.size) *available = off;
      continue;
    }
    int i = 0;
    while (i < DIRSIZ && ent.name[i] == name[i]) i++;
    if (i == DIRSIZ) {
      if (offset) *offset = off;
      return ent.inum;
    }
  }
  return 0;
}

struct inode *
dirlookup(struct inode *dir, const char *name, uint *offset)
{
  uint inum = dirfind(dir, name, offset, 0);
  return inum ? iget(inum) : 0;
}

int
dirlink(struct inode *dir, const char *name, uint inum)
{
  struct dirent ent = {.inum = inum};
  memmove(ent.name, name, DIRSIZ);
  uint off;
  if (dirfind(dir, name, 0, &off)) return -1;
  return iwrite(dir, 0, (uint64)&ent, off, sizeof(ent)) == sizeof(ent) ? 0 : -1;
}

static struct inode *
namex(const char *path, int parent, char *name)
{
  struct inode *ip = *path == '/' ? iget(ROOTINO) : idup(current_proc->cwd);
  if (!*path || !ip) {
    iput(ip);
    return 0;
  }
  while (*path == '/') path++;
  while (*path) {
    memset(name, 0, DIRSIZ);
    int len = 0;
    while (*path && *path != '/') {
      if (len == DIRSIZ) {
        iput(ip);
        return 0;
      }
      name[len++] = *path++;
    }
    while (*path == '/') path++;

    ilock(ip);
    if (ip->disk.type != T_DIR) {
      iunlock(ip);
      iput(ip);
      return 0;
    }
    if (parent && !*path) {
      iunlock(ip);
      return ip;
    }
    struct inode *next = dirlookup(ip, name, 0);
    iunlock(ip);
    iput(ip);
    if (!next) return 0;
    ip = next;
  }
  if (!parent) return ip;
  iput(ip);
  return 0;
}

struct inode *
namei(const char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *
nameiparent(const char *path, char *name)
{
  return namex(path, 1, name);
}
