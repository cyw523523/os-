#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc.h"

void syscall(void);
void consoleintr(void);
void printf(const char *, ...);
extern char kernelvec[];

static int
devintr(uint64 cause)
{
  if (cause == 0x8000000000000009ULL) {
    uint32 irq = *(volatile uint32 *)PLIC_SCLAIM(0);
    if (irq == UART0_IRQ)
      consoleintr();
    if (irq)
      *(volatile uint32 *)PLIC_SCLAIM(0) = irq;
    return 1;
  }
  return 0;
}

void
kerneltrap(void)
{
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 cause = r_scause();

  if (!devintr(cause)) {
    printf("kernel trap cause=%p epc=%p tval=%p\n",
           cause, sepc, r_stval());
    for (;;) asm volatile("wfi");
  }

  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
usertrap(void)
{
  uint64 cause = r_scause();
  w_stvec((uint64)kernelvec);
  current_proc->trapframe->epc = r_sepc();

  if (cause == 8) {
    current_proc->trapframe->epc += 4;
    syscall();
  } else if (!devintr(cause)) {
    proc_exit(-1);
  }
  switch_to(current_proc);
}
