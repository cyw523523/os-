#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"
#include "proc.h"

#include "buf.h"

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
    if (irq == VIRTIO0_IRQ) virtio_disk_intr();
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
  // A process can migrate after exec; each hart synchronizes its own I-cache.
  icache_fence();
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
  } else if (cause == 15) {
    if (cow_resolve(current_proc->pagetable, r_stval()) < 0) proc_exit(-1);
    // Retry the faulting store; epc must not advance as it does for ecall.
  } else if (!which) {
    proc_exit(-1);
  }
  if (__atomic_load_n(&current_proc->killed, __ATOMIC_ACQUIRE)) proc_exit(-1);
  if (which == 2 && current_proc->state == RUNNING &&
      current_proc->last_slice_ticks >= LAB2_TICK)
    yield();
  usertrapret();
}
