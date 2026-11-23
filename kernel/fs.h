#ifndef FS_H
#define FS_H

#define ROOTINO 1
#define BSIZE 1024
#define FSMAGIC 0x10203040
#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)
#define DIRSIZ 14

struct superblock {
  uint magic, size, nblocks, ninodes, nlog, logstart, inodestart, bmapstart;
};

struct dinode {
  short type, major, minor, nlink;
  uint size;
  uint addrs[NDIRECT + 1];
};

#define IPB (BSIZE / sizeof(struct dinode))
#define IBLOCK(i, sb) ((i) / IPB + (sb).inodestart)
#define BPB (BSIZE * 8)
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

#endif
