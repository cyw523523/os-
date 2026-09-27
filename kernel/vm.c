#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc.h"

#define PT_POOL_PAGES 96
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

static uchar pt_pool[PT_POOL_PAGES][PGSIZE] __attribute__((aligned(PGSIZE)));
static int pt_used;

static void
zero(void *dst, uint64 n)
{
  uchar *p = dst;
  while (n--)
    *p++ = 0;
}

static pagetable_t
ptalloc(void)
{
  if (pt_used == PT_POOL_PAGES)
    return 0;
  zero(pt_pool[pt_used], PGSIZE);
  return (pagetable_t)pt_pool[pt_used++];
}

static pte_t *
walk(pagetable_t root, uint64 va, int alloc)
{
  for (int level = 2; level > 0; level--) {
    pte_t *pte = &root[PX(level, va)];
    if (*pte & PTE_V)
      root = (pagetable_t)PTE2PA(*pte);
    else {
      if (!alloc || (root = ptalloc()) == 0)
        return 0;
      *pte = PA2PTE(root) | PTE_V;
    }
  }
  return &root[PX(0, va)];
}

static int
map(pagetable_t root, uint64 va, uint64 pa, uint64 size, int perm)
{
  uint64 end = PGROUNDUP(va + size);
  va = PGROUNDDOWN(va);
  pa = PGROUNDDOWN(pa);
  for (; va < end; va += PGSIZE, pa += PGSIZE) {
    pte_t *pte = walk(root, va, 1);
    if (pte == 0 || (*pte & PTE_V))
      return -1;
    *pte = PA2PTE(pa) | perm | PTE_A | PTE_D | PTE_V;
  }
  return 0;
}

static int
map2m(pagetable_t root, uint64 va, uint64 pa, uint64 size, int perm)
{
  pagetable_t level1 = ptalloc();
  if (level1 == 0 || (root[PX(2, va)] & PTE_V))
    return -1;
  root[PX(2, va)] = PA2PTE(level1) | PTE_V;
  for (uint64 off = 0; off < size; off += 2 * 1024 * 1024)
    level1[PX(1, va + off)] =
      PA2PTE(pa + off) | perm | PTE_A | PTE_D | PTE_V;
  return 0;
}

extern char trampoline[];

pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t root = ptalloc();
  if (root == 0)
    return 0;

  if (map2m(root, KERNBASE, KERNBASE, PHYSTOP - KERNBASE,
          PTE_R | PTE_W | PTE_X) < 0 ||
      map(root, UART0, UART0, PGSIZE, PTE_R | PTE_W) < 0 ||
      map(root, PLIC, PLIC, 0x400000, PTE_R | PTE_W) < 0 ||
      map(root, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W) < 0 ||
      map(root, 0, (uint64)p->memory, USER_MEM_SIZE,
          PTE_R | PTE_W | PTE_X | PTE_U) < 0 ||
      map(root, TRAPFRAME, (uint64)p->trapframe, PGSIZE,
          PTE_R | PTE_W) < 0 ||
      map(root, TRAMPOLINE, PGROUNDDOWN((uint64)trampoline), PGSIZE,
          PTE_R | PTE_X) < 0)
    return 0;
  return root;
}

uint64
proc_satp(struct proc *p)
{
  return MAKE_SATP(p->pagetable);
}
