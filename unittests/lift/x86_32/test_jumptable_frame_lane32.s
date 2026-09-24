// i386 ESP is the full-width architectural stack pointer.  The self-copy
// must retain the private frame cell holding the authenticated GOTPC base.
// The relative table uses the established GOTOFF model, so failure here
// isolates frame-epoch handling rather than an unsupported absolute table.
.text
.Lgotpc_text_base:
.p2align 2
.globl jt_i386_private_frame_spill
.type jt_i386_private_frame_spill, @function
jt_i386_private_frame_spill:
  pushl %ebp
  movl %esp, %ebp
  subl $8, %esp
  movl %esp, %esp
  call .Lprivate_spill_pc
.Lprivate_spill_pc:
  popl %eax
  .byte 0x05                       // addl imm32, %eax
.Lprivate_spill_gotpc:
  .long .Lprivate_spill_gotpc - .Lprivate_spill_pc
  .reloc .Lprivate_spill_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl %eax, (%esp)
  movl 8(%ebp), %ecx
  andl $1, %ecx
.Lprivate_spill_dispatch:
  cmpl $1, %ecx
  ja .Lprivate_spill_default
  movl (%esp), %ebx
  movl .Lprivate_spill_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_private_frame_spill_branch
jt_i386_private_frame_spill_branch:
  jmp *%eax
.Lprivate_spill_case0:
  pushl %ecx
  call jt_i386_private_frame_scalar
  addl $4, %esp
  movl $1, %ecx
  jmp .Lprivate_spill_dispatch
.Lprivate_spill_case1:
  movl $3201, %eax
  leave
  ret
.Lprivate_spill_default:
  movl $-1, %eax
  leave
  ret
.size jt_i386_private_frame_spill, .-jt_i386_private_frame_spill
.p2align 2
.globl jt_i386_private_frame_scalar
.type jt_i386_private_frame_scalar, @function
jt_i386_private_frame_scalar:
  movl 4(%esp), %eax
  addl $1, %eax
  ret
.size jt_i386_private_frame_scalar, .-jt_i386_private_frame_scalar
.section .rodata,"a",@progbits
.p2align 2
.globl jt_i386_private_frame_table
.type jt_i386_private_frame_table, @object
jt_i386_private_frame_table:
.Lprivate_spill_table:
  .long .Lprivate_spill_case0 - .Lgotpc_text_base
  .long .Lprivate_spill_case1 - .Lgotpc_text_base
.size jt_i386_private_frame_table, .-jt_i386_private_frame_table
