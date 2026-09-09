// Direct consumers may share a GOTOFF table, but not an adjacent table object.
.macro direct_gotoff_group name, separate
.text
.p2align 2
.globl \name
.type \name, @function
\name:
  call .L\name\()_pc
.L\name\()_pc:
  popl %ebx
  .byte 0x81, 0xc3
.L\name\()_gotpc:
  .long .L\name\()_gotpc - .L\name\()_pc
  .reloc .L\name\()_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %eax
  testl $2, %eax
  jnz .L\name\()_second
  andl $1, %eax
  jmp *\name\()_table@GOTOFF(%ebx,%eax,4)
.L\name\()_second:
  andl $1, %eax
.if \separate
  jmp *\name\()_adjacent@GOTOFF(%ebx,%eax,4)
.else
  orl $2, %eax
  jmp *\name\()_table@GOTOFF(%ebx,%eax,4)
.endif
.L\name\()_0: movl $10, %eax; ret
.L\name\()_1: movl $11, %eax; ret
.L\name\()_2: movl $12, %eax; ret
.L\name\()_3: movl $13, %eax; ret
.size \name, .-\name
.section .data.rel.ro,"aw",@progbits
.p2align 2
.type \name\()_table, @object
\name\()_table:
  .long .L\name\()_0, .L\name\()_1, .L\name\()_2, .L\name\()_3
.size \name\()_table, .-\name\()_table
.if \separate
.type \name\()_adjacent, @object
\name\()_adjacent:
  .long .L\name\()_0, .L\name\()_1, .L\name\()_2, .L\name\()_3
.size \name\()_adjacent, .-\name\()_adjacent
.endif
.endm

direct_gotoff_group jt_i386_direct_shared, 0
direct_gotoff_group jt_i386_direct_separate, 1

// The loop remainder precedes the dispatch block, whose entry seed is zero.
.macro prefix_modulo name, magic, bad_arm
.text
.p2align 2
.globl \name
.type \name, @function
\name:
  pushl %ebx
  pushl %edi
  pushl %esi
  movl 16(%esp), %ecx
  call .L\name\()_pc
.L\name\()_pc:
  popl %esi
  .byte 0x81, 0xc6
.L\name\()_gotpc:
  .long .L\name\()_gotpc - .L\name\()_pc
  .reloc .L\name\()_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl $\magic, %ebx
  movl $96, %edi
  orl $1, %ecx
  xorl %eax, %eax
  jmp .L\name\()_dispatch
.L\name\()_remainder:
  movl %ecx, %eax
  mull %ebx
  movl %ecx, %eax
  shrl $1, %edx
  andl $-4, %edx
  leal (%edx,%edx,2), %edx
  subl %edx, %eax
  decl %edi
  jz .L\name\()_done
.L\name\()_dispatch:
  jmp *\name\()_table@GOTOFF(%esi,%eax,4)
.L\name\()_0:
.if \bad_arm
  addl $2, %ecx
.else
  incl %ecx
.endif
  jmp .L\name\()_remainder
.irp n,1,2,3,4,5,6,7,8,9,10
.L\name\()_\n:
  addl $\n, %ecx
  jmp .L\name\()_remainder
.endr
.L\name\()_11:
.if \bad_arm
  movl $12, %eax
  jmp .L\name\()_dispatch
.else
  addl $11, %ecx
  jmp .L\name\()_remainder
.endif
.L\name\()_done:
  movl %ecx, %eax
  popl %esi
  popl %edi
  popl %ebx
  ret
.size \name, .-\name
.section .data.rel.ro,"aw",@progbits
.p2align 2
.type \name\()_table, @object
\name\()_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11
  .long .L\name\()_\n
.endr
.size \name\()_table, .-\name\()_table
.endm

prefix_modulo jt_i386_prefix_modulo, 0xaaaaaaab, 0
prefix_modulo jt_i386_prefix_bad_magic, 0xaaaaaaaa, 0
prefix_modulo jt_i386_prefix_bad_arm, 0xaaaaaaab, 1

.section .note.GNU-stack,"",@progbits
