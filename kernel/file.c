#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "param.h"
#include "fsinternal.h"
#include "defs.h"
#include "stat.h"
#include "fcntl.h"

struct file {
  struct sleeplock lock;
  int refs, readable, writable;
  uint offset;
  struct inode *ip;
};

static struct {
  struct spinlock lock;
  struct file entries[NFILE];
} files;

static struct sleeplock namespace;

static int
freefd(void)
{
  struct proc *p = current_proc;
  for (int fd = 3; fd < NOFILE; fd++)
    if (!p->pipes[fd] && !p->files[fd]) return fd;
  return -1;
}

static struct file *
getfile(int fd)
{
  if (fd < 3 || fd >= NOFILE) return 0;
  return current_proc->files[fd];
}

static struct file *
filealloc(void)
{
  acquire(&files.lock);
  for (int i = 0; i < NFILE; i++) {
    struct file *f = &files.entries[i];
    if (!f->refs) {
      f->refs = 1;
      f->ip = 0;
      release_lock(&files.lock);
      return f;
    }
  }
  release_lock(&files.lock);
  return 0;
}

static void
filedrop(struct file *f)
{
  acquire(&files.lock);
  if (--f->refs) {
    release_lock(&files.lock);
    return;
  }
  struct inode *ip = f->ip;
  f->ip = 0;
  release_lock(&files.lock);
  if (ip) {
    begin_op();
    iput(ip);
    end_op();
  }
}

static struct file *
filedup(struct file *f)
{
  acquire(&files.lock);
  f->refs++;
  release_lock(&files.lock);
  return f;
}

void
fs_bootstrap(struct proc *p)
{
  fsinit();
  p->cwd = iget(ROOTINO);
  if (!p->cwd) panic("root inode unavailable");
}

void
fs_fork(struct proc *parent, struct proc *child)
{
  child->cwd = idup(parent->cwd);
  for (int i = 3; i < NOFILE; i++)
    if (parent->files[i]) child->files[i] = filedup(parent->files[i]);
}

void
fs_exit(struct proc *p)
{
  for (int fd = 3; fd < NOFILE; fd++)
    if (p->files[fd]) fs_close(fd);
  begin_op();
  iput(p->cwd);
  p->cwd = 0;
  end_op();
}

static int
isdot(const char *name)
{
  return name[0] == '.' &&
         (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

static struct inode *
create(const char *path, short type)
{
  static const char dot[DIRSIZ] = ".", dotdot[DIRSIZ] = "..";
  char name[DIRSIZ];
  struct inode *dir = nameiparent(path, name);
  if (!dir) return 0;
  ilock(dir);
  struct inode *ip = dirlookup(dir, name, 0);
  if (ip) {
    iunlock(dir);
    iput(dir);
    ilock(ip);
    int regular = type == T_FILE && ip->disk.type == T_FILE;
    iunlock(ip);
    if (regular) return ip;
    iput(ip);
    return 0;
  }
  if (dir->disk.nlink == 0 || isdot(name)) goto fail_parent;

  ip = ialloc(type);
  if (!ip) goto fail_parent;
  ilock(ip);
  if (type == T_DIR &&
      (dirlink(ip, dot, ip->inum) < 0 || dirlink(ip, dotdot, dir->inum) < 0))
    goto fail_child;
  if (dirlink(dir, name, ip->inum) < 0) goto fail_child;

  ip->disk.nlink = 1;
  iupdate(ip);
  if (type == T_DIR) {
    dir->disk.nlink++;
    iupdate(dir);
  }
  iunlock(ip);
  iunlock(dir);
  iput(dir);
  return ip;

fail_child:
  iunlock(ip);
  iput(ip);
fail_parent:
  iunlock(dir);
  iput(dir);
  return 0;
}

int
fs_open(const char *path, int mode)
{
  if ((mode & ~(O_WRONLY | O_RDWR | O_CREATE | O_TRUNC)) ||
      (mode & (O_WRONLY | O_RDWR)) == (O_WRONLY | O_RDWR))
    return -1;
  int fd = freefd();
  if (fd < 0) return -1;
  struct file *f = filealloc();
  if (!f) return -1;
  begin_op();
  acquiresleep(&namespace);
  struct inode *ip = mode & O_CREATE ? create(path, T_FILE) : namei(path);
  releasesleep(&namespace);
  if (!ip) {
    end_op();
    filedrop(f);
    return -1;
  }
  ilock(ip);
  if (ip->disk.type != T_FILE && mode != O_RDONLY) {
    iunlock(ip);
    iput(ip);
    end_op();
    filedrop(f);
    return -1;
  }
  if (mode & O_TRUNC) itrunc(ip);
  iunlock(ip);
  end_op();

  f->ip = ip;
  f->readable = !(mode & O_WRONLY);
  f->writable = (mode & (O_WRONLY | O_RDWR)) != 0;
  f->offset = 0;
  current_proc->files[fd] = f;
  return fd;
}

int
fs_close(int fd)
{
  struct file *f = getfile(fd);
  if (!f) return pipeclose(fd);
  current_proc->files[fd] = 0;
  filedrop(f);
  return 0;
}

int
fs_dup(int fd)
{
  struct file *f = getfile(fd);
  if (!f) return -1;
  int next = freefd();
  if (next < 0) return -1;
  current_proc->files[next] = filedup(f);
  return next;
}

int
fs_read(int fd, uint64 dst, int n)
{
  struct file *f = getfile(fd);
  if (!f) return piperead(fd, dst, n);
  if (!f->readable || n < 0 || usercheck(current_proc->pagetable, dst, n, 1) < 0)
    return -1;
  acquiresleep(&f->lock);
  ilock(f->ip);
  int count = iread(f->ip, 1, dst, f->offset, n);
  if (count > 0) f->offset += count;
  iunlock(f->ip);
  releasesleep(&f->lock);
  return count;
}

int
fs_write(int fd, uint64 src, int n)
{
  struct file *f = getfile(fd);
  if (!f) return pipewrite(fd, src, n);
  if (!f->writable || n < 0 || usercheck(current_proc->pagetable, src, n, 0) < 0)
    return -1;
  int done = 0;
  while (done < n) {
    begin_op();
    acquiresleep(&f->lock);
    ilock(f->ip);
    int step = BSIZE - f->offset % BSIZE;
    if (step > n - done) step = n - done;
    int count = iwrite(f->ip, 1, src + done, f->offset, step);
    if (count > 0) f->offset += count;
    iunlock(f->ip);
    releasesleep(&f->lock);
    end_op();
    if (count < 0) return done ? done : -1;
    done += count;
  }
  return done;
}

int
fs_fstat(int fd, uint64 address)
{
  struct file *f = getfile(fd);
  if (!f) return -1;
  ilock(f->ip);
  struct stat st = {0};
  st.dev = ROOTDEV;
  st.ino = f->ip->inum;
  st.type = f->ip->disk.type;
  st.nlink = f->ip->disk.nlink;
  st.size = f->ip->disk.size;
  iunlock(f->ip);
  return usercopy(current_proc->pagetable, address, &st, sizeof(st), 1);
}

int
fs_mkdir(const char *path)
{
  begin_op();
  acquiresleep(&namespace);
  struct inode *ip = create(path, T_DIR);
  releasesleep(&namespace);
  iput(ip);
  end_op();
  return ip ? 0 : -1;
}

int
fs_chdir(const char *path)
{
  begin_op();
  acquiresleep(&namespace);
  struct inode *ip = namei(path);
  releasesleep(&namespace);
  int ok = 0;
  if (ip) {
    ilock(ip);
    ok = ip->disk.type == T_DIR;
    iunlock(ip);
  }
  if (ok) {
    iput(current_proc->cwd);
    current_proc->cwd = ip;
  } else {
    iput(ip);
  }
  end_op();
  return ok ? 0 : -1;
}

int
fs_link(const char *old, const char *new)
{
  begin_op();
  acquiresleep(&namespace);
  struct inode *ip = namei(old);
  char name[DIRSIZ];
  struct inode *dir = ip ? nameiparent(new, name) : 0;
  int result = -1;
  if (dir && dir != ip && !isdot(name)) {
    ilock(dir);
    ilock(ip);
    if (dir->disk.nlink > 0 && ip->disk.type == T_FILE &&
        ip->disk.nlink < 32767 && dirlink(dir, name, ip->inum) == 0) {
      ip->disk.nlink++;
      iupdate(ip);
      result = 0;
    }
    iunlock(ip);
    iunlock(dir);
  }
  iput(dir);
  iput(ip);
  releasesleep(&namespace);
  end_op();
  return result;
}

static int
isempty(struct inode *ip)
{
  struct dirent de;
  for (uint off = 2 * sizeof(de); off < ip->disk.size; off += sizeof(de))
    if (iread(ip, 0, (uint64)&de, off, sizeof(de)) != sizeof(de) || de.inum)
      return 0;
  return 1;
}

int
fs_unlink(const char *path)
{
  begin_op();
  acquiresleep(&namespace);
  char name[DIRSIZ];
  struct inode *dir = nameiparent(path, name);
  int result = -1;
  if (dir && !isdot(name)) {
    ilock(dir);
    uint off;
    struct inode *ip = dirlookup(dir, name, &off);
    if (ip) {
      ilock(ip);
      struct dirent empty = {0};
      if ((ip->disk.type != T_DIR || (inode_unreferenced(ip) && isempty(ip))) &&
          iwrite(dir, 0, (uint64)&empty, off, sizeof(empty)) == sizeof(empty)) {
        if (ip->disk.type == T_DIR) {
          dir->disk.nlink--;
          iupdate(dir);
        }
        ip->disk.nlink--;
        iupdate(ip);
        result = 0;
      }
      iunlock(ip);
      iput(ip);
    }
    iunlock(dir);
  }
  iput(dir);
  releasesleep(&namespace);
  end_op();
  return result;
}
