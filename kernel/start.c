#include "types.h"
#include "riscv.h"

void main(void);

void
start(void)
{
  uint64 x;

  // Return to supervisor mode when mret executes.
  x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);
  w_mepc((uint64)main);

  // Delegate traps that may arise after the privilege transition.
  w_medeleg(0xffff);
  w_mideleg(0xffff);

  // Permit S/U-mode access to the QEMU physical address space.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // Sstc: delegate the supervisor timer comparator to S mode.
  w_menvcfg(r_menvcfg() | MENVCFG_STCE);
  w_mcounteren(r_mcounteren() | 2);

  // Lab1 has no page tables yet: use physical addresses directly.
  w_satp(0);
  sfence_vma();

  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  asm volatile("mret");
  for (;;)
    asm volatile("wfi");
}
