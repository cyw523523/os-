#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "proc.h"

#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

extern char kernel_end[], trampoline[];
void printf(const char *, ...);

#define MAX_PHYSICAL_PAGES ((PHYSTOP - KERNBASE) / PGSIZE)
#define PAGE_WORDS (MAX_PHYSICAL_PAGES / 64)
static uint64 free_pages[PAGE_WORDS];
static uint64 free_groups[PAGE_WORDS / 64];
static uint64 free_top;
static uint64 first_page;
static uint64 allocated_count;
static uint64 page_limit;
static pagetable_t kernel_table;

uint64
kpages_used(void)
{
  return allocated_count;
}

static uint64
highest_bit(uint64 bits)
{
  uint64 index = 0;
  while (bits >>= 1)
    index++;
  return index;
}

static void
zero(void *dst, uint64 count)
{
  uchar *bytes = dst;
  while (count--)
    *bytes++ = 0;
}

void
kinit(void)
{
  first_page = PGROUNDUP((uint64)kernel_end);
  page_limit = (PHYSTOP - first_page) / PGSIZE - LAB3_MEMCAP_DELTA;
  for (uint64 address = first_page; address < PHYSTOP; address += PGSIZE) {
    uint64 index = (address - KERNBASE) / PGSIZE;
    uint64 word = index / 64;
    uint64 group = word / 64;
    free_pages[word] |= 1ULL << (index % 64);
    free_groups[group] |= 1ULL << (word % 64);
    free_top |= 1ULL << group;
  }
}

void *
kalloc(void)
{
  if (free_top == 0 || allocated_count >= page_limit)
    return 0;
  uint64 group = highest_bit(free_top);
  uint64 word = group * 64 + highest_bit(free_groups[group]);
  uint64 index = word * 64 + highest_bit(free_pages[word]);
  free_pages[word] &= ~(1ULL << (index % 64));
  if (free_pages[word] == 0) {
    free_groups[group] &= ~(1ULL << (word % 64));
    if (free_groups[group] == 0)
      free_top &= ~(1ULL << group);
  }
  allocated_count++;
  void *address = (void *)(KERNBASE + index * PGSIZE);
  zero(address, PGSIZE);
  return address;
}

void
kfree(void *address)
{
  uint64 page = (uint64)address;
  if (page < first_page || page >= PHYSTOP ||
      (page & (PGSIZE - 1)) != 0)
    return;
  uint64 index = (page - KERNBASE) / PGSIZE;
  uint64 word = index / 64;
  uint64 group = word / 64;
  uint64 mask = 1ULL << (index % 64);
  if (free_pages[word] & mask)
    return;
  free_pages[word] |= mask;
  free_groups[group] |= 1ULL << (word % 64);
  free_top |= 1ULL << group;
  allocated_count--;
}

static pte_t *
walk(pagetable_t root, uint64 va, int allocate)
{
  if (va >= MAXVA)
    return 0;
  for (int level = 2; level > 0; level--) {
    pte_t *pte = &root[PX(level, va)];
    if (*pte & PTE_V) {
      if (*pte & (PTE_R | PTE_W | PTE_X))
        return 0;
      root = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!allocate)
        return 0;
      pagetable_t child = kalloc();
      if (child == 0)
        return 0;
      *pte = PA2PTE(child) | PTE_V;
      root = child;
    }
  }
  return &root[PX(0, va)];
}

static int
map(pagetable_t root, uint64 va, uint64 pa, uint64 size, int flags)
{
  if (size == 0 || va >= MAXVA || size > MAXVA - va)
    return -1;
  uint64 start = PGROUNDDOWN(va);
  uint64 end = PGROUNDUP(va + size);
  for (uint64 address = start; address < end; address += PGSIZE) {
    pte_t *pte = walk(root, address, 1);
    if (pte == 0 || (*pte & PTE_V)) {
      for (uint64 undo = start; undo < address; undo += PGSIZE) {
        pte_t *old = walk(root, undo, 0);
        if (old)
          *old = 0;
      }
      return -1;
    }
    *pte = PA2PTE(pa + address - start) | flags | PTE_A | PTE_D | PTE_V;
  }
  return 0;
}

static int
map2m(pagetable_t root, uint64 va, uint64 pa, uint64 size, int flags)
{
  pagetable_t level1 = kalloc();
  if (level1 == 0)
    return -1;
  root[PX(2, va)] = PA2PTE(level1) | PTE_V;
  for (uint64 offset = 0; offset < size; offset += 2 * 1024 * 1024)
    level1[PX(1, va + offset)] =
      PA2PTE(pa + offset) | flags | PTE_A | PTE_D | PTE_V;
  return 0;
}

static void
freewalk(pagetable_t root, int level)
{
  for (int index = 0; index < 512; index++) {
    pte_t pte = root[index];
    if (!(pte & PTE_V))
      continue;
    if (level && !(pte & (PTE_R | PTE_W | PTE_X)))
      freewalk((pagetable_t)PTE2PA(pte), level - 1);
    else if (pte & PTE_U)
      kfree((void *)PTE2PA(pte));
  }
  kfree(root);
}

static int
prune_empty(pagetable_t root, int level)
{
  int populated = 0;
  for (int index = 0; index < 512; index++) {
    pte_t pte = root[index];
    if (!(pte & PTE_V))
      continue;
    if (level && !(pte & (PTE_R | PTE_W | PTE_X))) {
      pagetable_t child = (pagetable_t)PTE2PA(pte);
      if (!prune_empty(child, level - 1)) {
        root[index] = 0;
        kfree(child);
        continue;
      }
    }
    populated = 1;
  }
  return populated;
}

void
proc_freepagetable(pagetable_t root)
{
  if (root)
    freewalk(root, 2);
}

pagetable_t
proc_pagetable(struct proc *process)
{
  pagetable_t root = kalloc();
  if (root == 0)
    return 0;
  if (map(root, TRAPFRAME, (uint64)process->trapframe, PGSIZE,
          PTE_R | PTE_W) < 0 ||
      map(root, TRAMPOLINE, PGROUNDDOWN((uint64)trampoline), PGSIZE,
          PTE_R | PTE_X) < 0) {
    proc_freepagetable(root);
    return 0;
  }
  return root;
}

void
kvminit(void)
{
  kernel_table = kalloc();
  if (kernel_table == 0 ||
      map2m(kernel_table, KERNBASE, KERNBASE, PHYSTOP - KERNBASE,
            PTE_R | PTE_W | PTE_X) < 0 ||
      map(kernel_table, UART0, UART0, PGSIZE, PTE_R | PTE_W) < 0 ||
      map(kernel_table, PLIC, PLIC, 0x400000, PTE_R | PTE_W) < 0 ||
      map(kernel_table, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W) < 0 ||
      map(kernel_table, TRAMPOLINE, PGROUNDDOWN((uint64)trampoline),
          PGSIZE, PTE_R | PTE_X) < 0)
    for (;;) asm volatile("wfi");
  w_satp(MAKE_SATP(kernel_table));
  sfence_vma();
}

uint64
kernel_satp(void)
{
  return MAKE_SATP(kernel_table);
}

int
uvmalloc(pagetable_t root, uint64 start, uint64 end, int flags)
{
  uint64 address = PGROUNDUP(start);
  for (; address < PGROUNDUP(end); address += PGSIZE) {
    void *page = kalloc();
    if (page == 0 || map(root, address, (uint64)page, PGSIZE, flags | PTE_U) < 0) {
      if (page)
        kfree(page);
      for (uint64 undo = PGROUNDUP(start); undo < address; undo += PGSIZE) {
        pte_t *pte = walk(root, undo, 0);
        if (pte && (*pte & PTE_V)) {
          uint64 physical = PTE2PA(*pte);
          *pte = 0;
          kfree((void *)physical);
        }
      }
      prune_empty(root, 2);
      return -1;
    }
  }
  return 0;
}

void
uvmdealloc(pagetable_t root, uint64 start, uint64 end)
{
  for (uint64 address = PGROUNDUP(start); address < PGROUNDUP(end);
       address += PGSIZE) {
    pte_t *pte = walk(root, address, 0);
    if (pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U))
      continue;
    uint64 physical = PTE2PA(*pte);
    *pte = 0;
    kfree((void *)physical);
  }
  prune_empty(root, 2);
}

int
uvmcopy(pagetable_t source, pagetable_t target, uint64 start, uint64 end)
{
  for (uint64 va = PGROUNDDOWN(start); va < PGROUNDUP(end); va += PGSIZE) {
    pte_t *pte = walk(source, va, 0);
    if (pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U))
      continue;
    void *page = kalloc();
    if (page == 0)
      return -1;
    uchar *from = (uchar *)PTE2PA(*pte);
    uchar *to = page;
    for (int index = 0; index < PGSIZE; index++)
      to[index] = from[index];
    if (map(target, va, (uint64)page, PGSIZE, PTE_FLAGS(*pte) & ~PTE_V) < 0) {
      kfree(page);
      return -1;
    }
  }
  return 0;
}

static uint64
userpa(pagetable_t root, uint64 va, int write)
{
  pte_t *pte = walk(root, va, 0);
  if (pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U) ||
      !(*pte & (write ? PTE_W : PTE_R)))
    return 0;
  return PTE2PA(*pte) + (va & (PGSIZE - 1));
}

int
usercheck(pagetable_t root, uint64 va, uint64 count, int write)
{
  if (count == 0)
    return 0;
  if (va >= MAXVA || count > MAXVA - va)
    return -1;
  for (uint64 offset = 0; offset < count;) {
    if (userpa(root, va + offset, write) == 0)
      return -1;
    uint64 chunk = PGSIZE - ((va + offset) & (PGSIZE - 1));
    if (chunk > count - offset)
      chunk = count - offset;
    offset += chunk;
  }
  return 0;
}

int
usercopy(pagetable_t root, uint64 va, void *buffer, uint64 count, int to_user)
{
  if (count == 0)
    return 0;
  if (va >= MAXVA || count > MAXVA - va)
    return -1;
  uchar *bytes = buffer;
  for (uint64 offset = 0; offset < count;) {
    uint64 physical = userpa(root, va + offset, to_user);
    if (physical == 0)
      return -1;
    uint64 chunk = PGSIZE - ((va + offset) & (PGSIZE - 1));
    if (chunk > count - offset)
      chunk = count - offset;
    for (uint64 index = 0; index < chunk; index++) {
      if (to_user)
        ((uchar *)physical)[index] = bytes[offset + index];
      else
        bytes[offset + index] = ((uchar *)physical)[index];
    }
    offset += chunk;
  }
  return 0;
}

uint64
proc_satp(struct proc *process)
{
  return MAKE_SATP(process->pagetable);
}

static void
dumpwalk(pagetable_t root, int level, uint64 base)
{
  for (int index = 0; index < 512; index++) {
    pte_t pte = root[index];
    if (!(pte & PTE_V))
      continue;
    uint64 va = base + ((uint64)index << PXSHIFT(level));
    uint64 pa = PTE2PA(pte);
    char flags[] = "----";
    if (pte & PTE_R) flags[0] = 'r';
    if (pte & PTE_W) flags[1] = 'w';
    if (pte & PTE_X) flags[2] = 'x';
    if (pte & PTE_U) flags[3] = 'u';
    printf("%s va=0x%p pa=0x%p perm=%s\n",
           (pte & (PTE_R | PTE_W | PTE_X)) ? "LEAF" :
           level == 2 ? "L2" : "L1", va, pa, flags);
    if (level && !(pte & (PTE_R | PTE_W | PTE_X)))
      dumpwalk((pagetable_t)pa, level - 1, va);
  }
}

void
dump_pagetable(pagetable_t root)
{
  printf("DUMP-PAGETABLE begin\n");
  dumpwalk(root, 2, 0);
  printf("DUMP-PAGETABLE end\n");
}
