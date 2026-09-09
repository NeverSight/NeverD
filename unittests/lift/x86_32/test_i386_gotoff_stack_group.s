// An independent reader is reachable only after the second dispatch. Its
// returned code pointer must retain the first table's relocation root.
.text
.globl jt_i386_gotoff_consumer_stack_tables
.type jt_i386_gotoff_consumer_stack_tables, @function
jt_i386_gotoff_consumer_stack_tables:
  pushl %ebp
  movl %esp, %ebp
  subl $8, %esp
  call .Loconsumer_pc
.Loconsumer_pc:
  popl %eax
  .byte 0x05
.Loconsumer_gotfield:
  .long .Loconsumer_gotfield - .Loconsumer_pc
  .reloc .Loconsumer_gotfield, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
.globl jt_i386_gotoff_consumer_stack_got_store
jt_i386_gotoff_consumer_stack_got_store:
  movl %eax, -4(%ebp)
.Loconsumer_loop:
  movl 8(%ebp), %eax
  andl $7, %eax
  movl %eax, -8(%ebp)
  movl -8(%ebp), %eax
.globl jt_i386_gotoff_consumer_stack_first_guard
jt_i386_gotoff_consumer_stack_first_guard:
  cmpl $6, %eax
  ja .Loconsumer_second
  movl -4(%ebp), %ecx
  movl .Loconsumer_table1@GOTOFF(%ecx,%eax,4), %eax
  addl %ecx, %eax
.globl jt_i386_gotoff_consumer_stack_first_branch
jt_i386_gotoff_consumer_stack_first_branch:
  jmp *%eax
.Loconsumer_first0:
  jmp .Loconsumer_second
.Loconsumer_first1:
  jmp .Loconsumer_second
.Loconsumer_first2:
  jmp .Loconsumer_second
.Loconsumer_first3:
  jmp .Loconsumer_second
.Loconsumer_first4:
  jmp .Loconsumer_second
.Loconsumer_first5:
  jmp .Loconsumer_second
.Loconsumer_first6:
  jmp .Loconsumer_second
.Loconsumer_second:
  movl -8(%ebp), %eax
.globl jt_i386_gotoff_consumer_stack_second_guard
jt_i386_gotoff_consumer_stack_second_guard:
  cmpl $6, %eax
  ja .Loconsumer_loop
  movl -4(%ebp), %ecx
  movl .Loconsumer_table2@GOTOFF(%ecx,%eax,4), %eax
  addl %ecx, %eax
.globl jt_i386_gotoff_consumer_stack_second_branch
jt_i386_gotoff_consumer_stack_second_branch:
  jmp *%eax
.Loconsumer_last0:
  movl -4(%ebp), %ecx
  movl .Loconsumer_table1@GOTOFF(%ecx), %eax
  leave
  ret
.Loconsumer_last1:
  jmp .Loconsumer_loop
.Loconsumer_last2:
  jmp .Loconsumer_loop
.Loconsumer_last3:
  jmp .Loconsumer_loop
.Loconsumer_last4:
  jmp .Loconsumer_loop
.Loconsumer_last5:
  jmp .Loconsumer_loop
.Loconsumer_last6:
  jmp .Loconsumer_loop
.size jt_i386_gotoff_consumer_stack_tables, .-jt_i386_gotoff_consumer_stack_tables
.section .rodata.o0consumer,"a",@progbits
.p2align 2
.globl jt_i386_gotoff_consumer_stack_first_table
jt_i386_gotoff_consumer_stack_first_table:
.Loconsumer_table1:
  .long .Loconsumer_first0@GOTOFF
  .long .Loconsumer_first1@GOTOFF
  .long .Loconsumer_first2@GOTOFF
  .long .Loconsumer_first3@GOTOFF
  .long .Loconsumer_first4@GOTOFF
  .long .Loconsumer_first5@GOTOFF
  .long .Loconsumer_first6@GOTOFF
.globl jt_i386_gotoff_consumer_stack_second_table
jt_i386_gotoff_consumer_stack_second_table:
.Loconsumer_table2:
  .long .Loconsumer_last0@GOTOFF
  .long .Loconsumer_last1@GOTOFF
  .long .Loconsumer_last2@GOTOFF
  .long .Loconsumer_last3@GOTOFF
  .long .Loconsumer_last4@GOTOFF
  .long .Loconsumer_last5@GOTOFF
  .long .Loconsumer_last6@GOTOFF


// Keep this owner last: the independent-pointer regression extends its tail.
// Two different table owners feed the same loop. Both GOT and selector values
// cross stack slots; every case returns to a dispatch instead of terminating.
.text
.globl jt_i386_gotoff_two_stack_tables
.type jt_i386_gotoff_two_stack_tables, @function
jt_i386_gotoff_two_stack_tables:
  pushl %ebp
  movl %esp, %ebp
  subl $8, %esp
  call .Lo0_pc
.Lo0_pc:
  popl %eax
  .byte 0x05
.Lo0_gotfield:
  .long .Lo0_gotfield - .Lo0_pc
  .reloc .Lo0_gotfield, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
.globl jt_i386_gotoff_two_stack_got_store
jt_i386_gotoff_two_stack_got_store:
  movl %eax, -4(%ebp)
.Lo0_loop:
  movl 8(%ebp), %eax
  andl $7, %eax
  movl %eax, -8(%ebp)
  movl -8(%ebp), %eax
.globl jt_i386_gotoff_two_stack_first_guard
jt_i386_gotoff_two_stack_first_guard:
  cmpl $6, %eax
  ja .Lo0_second
  movl -4(%ebp), %ecx
  movl .Lo0_table1@GOTOFF(%ecx,%eax,4), %eax
  addl %ecx, %eax
.globl jt_i386_gotoff_two_stack_first_branch
jt_i386_gotoff_two_stack_first_branch:
  jmp *%eax
.Lo0_first0:
  jmp .Lo0_second
.Lo0_first1:
  jmp .Lo0_second
.Lo0_first2:
  jmp .Lo0_second
.Lo0_first3:
  jmp .Lo0_second
.Lo0_first4:
  jmp .Lo0_second
.Lo0_first5:
  jmp .Lo0_second
.Lo0_first6:
  jmp .Lo0_second
.Lo0_second:
  movl -8(%ebp), %eax
.globl jt_i386_gotoff_two_stack_second_guard
jt_i386_gotoff_two_stack_second_guard:
  cmpl $6, %eax
  ja .Lo0_loop
  movl -4(%ebp), %ecx
  movl .Lo0_table2@GOTOFF(%ecx,%eax,4), %eax
  addl %ecx, %eax
.globl jt_i386_gotoff_two_stack_second_branch
jt_i386_gotoff_two_stack_second_branch:
  jmp *%eax
.Lo0_last0:
  jmp .Lo0_loop
.Lo0_last1:
  jmp .Lo0_loop
.Lo0_last2:
  jmp .Lo0_loop
.Lo0_last3:
  jmp .Lo0_loop
.Lo0_last4:
  jmp .Lo0_loop
.Lo0_last5:
  jmp .Lo0_loop
.Lo0_last6:
  jmp .Lo0_loop
.size jt_i386_gotoff_two_stack_tables, .-jt_i386_gotoff_two_stack_tables
.section .rodata.o0,"a",@progbits
.p2align 2
.globl jt_i386_gotoff_two_stack_first_table
jt_i386_gotoff_two_stack_first_table:
.Lo0_table1:
  .long .Lo0_first0@GOTOFF
  .long .Lo0_first1@GOTOFF
  .long .Lo0_first2@GOTOFF
  .long .Lo0_first3@GOTOFF
  .long .Lo0_first4@GOTOFF
  .long .Lo0_first5@GOTOFF
  .long .Lo0_first6@GOTOFF
.globl jt_i386_gotoff_two_stack_second_table
jt_i386_gotoff_two_stack_second_table:
.Lo0_table2:
  .long .Lo0_last0@GOTOFF
  .long .Lo0_last1@GOTOFF
  .long .Lo0_last2@GOTOFF
  .long .Lo0_last3@GOTOFF
  .long .Lo0_last4@GOTOFF
  .long .Lo0_last5@GOTOFF
  .long .Lo0_last6@GOTOFF

.section .note.GNU-stack,"",@progbits
