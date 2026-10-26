#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "proc.h"

void syscall(void);
void consoleintr(void);
void printf(const char *, ...);
void usertrap(void);
extern char kernelvec[], uservec[], userret[];

static int
devintr(uint64 cause)
{
  if (cause == 0x8000000000000005ULL) {
    w_stimecmp(r_time() + 100000);
    proc_tick();
    return 2;
  }
  if (cause == 0x8000000000000009ULL) {
    uint32 irq = *(volatile uint32 *)PLIC_SCLAIM(r_tp());
    if (irq == UART0_IRQ) consoleintr();
    if (irq) *(volatile uint32 *)PLIC_SCLAIM(r_tp()) = irq;
    return 1;
  }
  return 0;
}

void timerinit(void) { w_stimecmp(r_time() + 100000); }

void
kerneltrap(void)
{
  uint64 sepc = r_sepc(), sstatus = r_sstatus(), cause = r_scause();
  if (!devintr(cause)) {
    printf("kernel trap cause=%p epc=%p tval=%p\n", cause, sepc, r_stval());
    for (;;) asm volatile("wfi");
  }
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
usertrapret(void)
{
  struct proc *p = current_proc;
  intr_off();
  p->trapframe->kernel_satp = kernel_satp();
  p->trapframe->kernel_sp = (uint64)p->kstack + PGSIZE;
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();
  uint64 status = r_sstatus();
  status &= ~SSTATUS_SPP;
  status |= SSTATUS_SPIE;
  w_sstatus(status);
  w_sepc(p->trapframe->epc);
  w_stvec(TRAMPOLINE + ((uint64)uservec & (PGSIZE - 1)));
  ((void (*)(uint64))(TRAMPOLINE + ((uint64)userret & (PGSIZE - 1))))
    (proc_satp(p));
  for (;;) ;
}

void
usertrap(void)
{
  uint64 cause = r_scause();
  w_stvec((uint64)kernelvec);
  current_proc->trapframe->epc = r_sepc();
  int which = devintr(cause);
  if (cause == 8) {
    current_proc->trapframe->epc += 4;
    syscall();
  } else if (!which) {
    proc_exit(-1);
  }
  if (current_proc->killed) proc_exit(-1);
  if (which == 2 && current_proc->state == RUNNING &&
      current_proc->last_slice_ticks >= LAB2_TICK)
    yield();
  usertrapret();
}
