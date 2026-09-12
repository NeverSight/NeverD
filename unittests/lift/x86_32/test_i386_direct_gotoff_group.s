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

// A peeled dispatch and a loop dispatch share one table, followed by a
// second table. There are no selector comparison guards. The first selector
// can be only 1 or 3, while the mask still proves the universal [0,4) envelope.
.macro dense_mask_group name, mode
.text
.p2align 2
.globl \name
.type \name, @function
\name:
  pushl %ebx
  pushl %esi
  movl 12(%esp), %eax
  call .L\name\()_pc
.L\name\()_pc:
  popl %ecx
  .byte 0x81, 0xc1
.L\name\()_gotpc:
  .long .L\name\()_gotpc - .L\name\()_pc
  .reloc .L\name\()_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl $80, %esi
  orl $1, %eax
.if \mode == 1
  testl $8, %eax
  jnz .L\name\()_first_load
.endif
.if \mode == 4
  andl $5, %eax
.elseif \mode == 6
  andb $3, %al
.else
  andl $3, %eax
.endif
.if \mode == 2
  movl $4, %eax
.elseif \mode == 3
  movb $4, %al
.elseif \mode == 5
  decl %eax
.endif
.globl \name\()_first_load
\name\()_first_load:
.L\name\()_first_load:
  movl \name\()_first_table@GOTOFF(%ecx,%eax,4), %ebx
  addl %ecx, %ebx
.globl \name\()_first_branch
\name\()_first_branch:
  jmp *%ebx
.L\name\()_repeat:
  decl %esi
  jz .L\name\()_done
  movl %eax, %edx
  andl $3, %edx
  movl \name\()_first_table@GOTOFF(%ecx,%edx,4), %ebx
  addl %ecx, %ebx
  jmp *%ebx
.irp n,0,1,2,3
.L\name\()_first\n:
  addl $\n+1, %eax
  jmp .L\name\()_second
.endr
.L\name\()_second:
  movl %eax, %edx
  shrl $2, %edx
  andl $3, %edx
  movl \name\()_second_table@GOTOFF(%ecx,%edx,4), %ebx
  addl %ecx, %ebx
  jmp *%ebx
.irp n,0,1,2
.L\name\()_last\n:
  addl $\n+5, %eax
  jmp .L\name\()_repeat
.endr
.L\name\()_last3:
.if \mode == 8
  movl \name\()_first_table@GOTOFF(%ecx), %eax
  jmp .L\name\()_done
.else
  xorl $0x55, %eax
  jmp .L\name\()_repeat
.endif
.L\name\()_done:
  popl %esi
  popl %ebx
  ret
.size \name, .-\name
.section .rodata.\name,"a",@progbits
.p2align 2
.globl \name\()_first_table
\name\()_first_table:
.irp n,0,1,2,3
  .long .L\name\()_first\n@GOTOFF
.endr
.globl \name\()_second_table
\name\()_second_table:
.irp n,0,1,2,3
  .long .L\name\()_last\n@GOTOFF
.endr
.if \mode == 7
  .long .L\name\()_first0@GOTOFF
.endif
.endm

dense_mask_group jt_i386_mask_group, 0
dense_mask_group jt_i386_mask_bypass, 1
dense_mask_group jt_i386_mask_overwrite, 2
dense_mask_group jt_i386_mask_partial_write, 3
dense_mask_group jt_i386_mask_sparse, 4
dense_mask_group jt_i386_mask_decrement, 5
dense_mask_group jt_i386_mask_partial_mask, 6
dense_mask_group jt_i386_mask_extra_slot, 7
dense_mask_group jt_i386_mask_independent_reader, 8

.section .note.GNU-stack,"",@progbits
