#ifndef LAB5_H
#define LAB5_H

// Software bit in Sv39 RSW; never grants write access to genuine RO pages.
#define PTE_COW (1L << 8)
#define LAB5_MAXARG 32
#define SYS_pagecount 27 // private regression interface; course pmc is 26

struct elf64_header {
  uint magic;
  uchar ident[12];
  ushort type, machine;
  uint version;
  uint64 entry, phoff, shoff;
  uint flags;
  ushort ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_program {
  uint type, flags;
  uint64 offset, vaddr, paddr, filesz, memsz, align;
};

#endif
