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

  # Lab1 is a single-core baseline. Park every non-boot hart.
  csrr t0, mhartid
  bnez t0, park

  la sp, stack0_top
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
  .space LAB1_STACK_KB * 1024
.globl stack0_top
stack0_top:
