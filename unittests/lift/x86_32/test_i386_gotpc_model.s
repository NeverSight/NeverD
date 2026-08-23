.text
.p2align 2
.Lgotpc_text_base:

// A real i386 PIC get-PC seed.  The relocation addend is expressed so the
// loader's model-zero convention expects the exact address pushed by the
// call-next instruction.
.globl jt_i386_gotpc_call_pop_seed
.type jt_i386_gotpc_call_pop_seed, @function
jt_i386_gotpc_call_pop_seed:
  call .Lgotpc_real_pc
.Lgotpc_real_pc:
  popl %esi
  .byte 0x81, 0xc6                 // addl imm32, %esi
.Lgotpc_real_field:
  .long .Lgotpc_real_field - .Lgotpc_real_pc
  .reloc .Lgotpc_real_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl %esi, %eax
  ret
.size jt_i386_gotpc_call_pop_seed, .-jt_i386_gotpc_call_pop_seed

// The same numeric seed materialized by a relocation-free absolute LEA is not
// a get-PC occurrence.  It remains fixed when the object is linked elsewhere,
// so pairing it with R_386_GOTPC must not authenticate the model-zero result.
.p2align 2
.globl jt_i386_gotpc_absolute_seed
.type jt_i386_gotpc_absolute_seed, @function
jt_i386_gotpc_absolute_seed:
  leal .Lgotpc_fake_pc - .Lgotpc_text_base, %esi
.Lgotpc_fake_pc:
  .byte 0x81, 0xc6                 // addl imm32, %esi
.Lgotpc_fake_field:
  .long .Lgotpc_fake_field - .Lgotpc_fake_pc
  .reloc .Lgotpc_fake_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl %esi, %eax
  ret
.size jt_i386_gotpc_absolute_seed, .-jt_i386_gotpc_absolute_seed

.section .note.GNU-stack,"",@progbits
