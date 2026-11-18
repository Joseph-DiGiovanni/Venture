// AAPCS64 (Linux): x0-x7 and d0-d7 allocated independently; overflow in source
// order, one 8-byte slot each. The native stub (java/native_bridge.cpp) loads
// the method record into x16 and branches here. Captures java::HostArguments:
// gp[8] at 0, fp[8] at 64, stack at 128 (java/abi.h).
.text
.globl venture_java_native_entry
.type venture_java_native_entry,%function
.balign 4
venture_java_native_entry:
  .cfi_startproc
  hint #34                         // bti c (the stub reaches us through br)
  stp x29, x30, [sp, #-160]!
  .cfi_def_cfa_offset 160
  .cfi_offset x29, -160
  .cfi_offset x30, -152
  mov x29, sp
  stp x0, x1, [sp, #16]
  stp x2, x3, [sp, #32]
  stp x4, x5, [sp, #48]
  stp x6, x7, [sp, #64]
  stp d0, d1, [sp, #80]
  stp d2, d3, [sp, #96]
  stp d4, d5, [sp, #112]
  stp d6, d7, [sp, #128]
  add x9, sp, #160                 // caller's first stack argument
  str x9, [sp, #144]
  mov x0, x16
  add x1, sp, #16
  bl venture_java_native_dispatch
  fmov d0, x0                      // float/double results; integer results stay in x0
  ldp x29, x30, [sp], #160
  .cfi_def_cfa_offset 0
  .cfi_restore x29
  .cfi_restore x30
  ret
  .cfi_endproc
.size venture_java_native_entry, .-venture_java_native_entry
.section .note.GNU-stack,"",%progbits
