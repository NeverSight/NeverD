# Jumps through relative tables of a function's own code that nothing
# bounds, as Rust compiles a `match` on an enum's discriminant read from
# memory: `mov (%rsi), %rax; lea table(%rip), %rcx; movslq (%rcx,%rax,4),
# %rdx; add %rcx, %rdx; jmp *%rdx`.  No guard proves an index domain, so no
# table is published; the jump dispatches by value on the labels the table's
# leading slots name and traps on any other value.  Every table ends with a
# zero word, which names the table itself and so no code of the function.
#
# jt_own_enum names three labels in six slots.  jt_own_kept names one label
# whose code reads %rbx, which its caller keeps and no path to the jump has
# written: that edge would make %rbx an input, so only the other label is
# kept.  jt_own_inside names, in its second slot, an address inside an
# instruction, which ends what the table is taken to hold.  jt_own_writable
# reads its table from writable memory, which the program may change, and
# stays unresolved.

  .text
  .globl jt_own_enum
  .type jt_own_enum, @function
  .p2align 4
jt_own_enum:
  movq (%rsi), %rax
  leaq .Loe_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  jmpq *%rdx
.Loe_word:
  movq 8(%rsi), %rcx
  movq %rcx, 8(%rdi)
  movq %rax, (%rdi)
  retq
.Loe_byte:
  movzbl 8(%rsi), %ecx
  movb %cl, 8(%rdi)
  movq %rax, (%rdi)
  retq
.Loe_half:
  movzwl 8(%rsi), %ecx
  movw %cx, 8(%rdi)
  movq %rax, (%rdi)
  retq
  .size jt_own_enum, .-jt_own_enum

  .globl jt_own_kept
  .type jt_own_kept, @function
  .p2align 4
jt_own_kept:
  movq (%rdi), %rax
  leaq .Lok_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  jmpq *%rdx
.Lok_plain:
  movl $1, %eax
  retq
.Lok_reads_kept:
  movq %rbx, %rax
  retq
  .size jt_own_kept, .-jt_own_kept

  .globl jt_own_inside
  .type jt_own_inside, @function
  .p2align 4
jt_own_inside:
  movq (%rdi), %rax
  leaq .Loi_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  jmpq *%rdx
.Loi_first:
  movl $1, %eax
  retq
.Loi_wide:
  movabsq $0x1122334455667788, %rax
  retq
  .size jt_own_inside, .-jt_own_inside

  .globl jt_own_writable
  .type jt_own_writable, @function
  .p2align 4
jt_own_writable:
  movq (%rdi), %rax
  leaq .Low_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  jmpq *%rdx
.Low_first:
  movl $1, %eax
  retq
.Low_second:
  movl $2, %eax
  retq
  .size jt_own_writable, .-jt_own_writable

  .section .rodata
  .p2align 2
.Loe_table:
  .long .Loe_word-.Loe_table
  .long .Loe_byte-.Loe_table
  .long .Loe_word-.Loe_table
  .long .Loe_half-.Loe_table
  .long .Loe_byte-.Loe_table
  .long .Loe_word-.Loe_table
  .long 0
.Lok_table:
  .long .Lok_plain-.Lok_table
  .long .Lok_reads_kept-.Lok_table
  .long .Lok_plain-.Lok_table
  .long 0
.Loi_table:
  .long .Loi_first-.Loi_table
  .long .Loi_wide+2-.Loi_table
  .long .Loi_first-.Loi_table
  .long 0

  .data
  .p2align 2
.Low_table:
  .long .Low_first-.Low_table
  .long .Low_second-.Low_table
  .long 0

  .section .note.GNU-stack,"",@progbits
