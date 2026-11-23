#ifndef FSINTERNAL_H
#define FSINTERNAL_H

#include "fs.h"
#include "sleeplock.h"

struct inode {
  uint inum;
  int refs, valid;
  struct sleeplock lock;
  struct dinode disk;
};

struct stat;
struct proc;
void fsinit(void);
void fs_bootstrap(struct proc *);
void fs_fork(struct proc *, struct proc *);
void fs_exit(struct proc *);
int fs_open(const char *, int);
int fs_close(int);
int fs_read(int, uint64, int);
int fs_write(int, uint64, int);
int fs_fstat(int, uint64);
int fs_mkdir(const char *);
int fs_chdir(const char *);
int fs_dup(int);
int fs_link(const char *, const char *);
int fs_unlink(const char *);

struct inode *iget(uint);
struct inode *idup(struct inode *);
void ilock(struct inode *);
void iunlock(struct inode *);
void iput(struct inode *);
int inode_unreferenced(struct inode *);
void iupdate(struct inode *);
void itrunc(struct inode *);
int iread(struct inode *, int, uint64, uint, uint);
int iwrite(struct inode *, int, uint64, uint, uint);
struct inode *ialloc(short);
struct inode *dirlookup(struct inode *, const char *, uint *);
int dirlink(struct inode *, const char *, uint);
struct inode *namei(const char *);
struct inode *nameiparent(const char *, char *);

struct buf;
void loginit(const struct superblock *);
void log_write(struct buf *);
void begin_op(void);
void end_op(void);
void log_sync(void);
int log_arm_crash(int);
void crash_at(int);

#endif
