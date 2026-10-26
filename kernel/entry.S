#include "course_sid.h"

.section .text.entry
.globl _entry
.type _entry, @function
_entry:
  # Keep machine interrupts disabled until the privilege transition is ready.
  csrrci t0, mstatus, 0x8

  # Unexpected early machine traps must not jump through an invalid address.
  la t0, machine_trap
  csrw mtvec, t0

  # Two harts use separate boot/scheduler stacks.
  csrr t0, mhartid
  li t1, 2
  bgeu t0, t1, park

  la sp, stack0_top
  li t1, LAB1_STACK_KB * 1024
  mul t0, t0, t1
  sub sp, sp, t0
  call start

park:
  wfi
  j park

.balign 4
machine_trap:
  j machine_trap

.section .bss.stack
.balign 16
stack0:
  .space 2 * LAB1_STACK_KB * 1024
.globl stack0_top
stack0_top:
