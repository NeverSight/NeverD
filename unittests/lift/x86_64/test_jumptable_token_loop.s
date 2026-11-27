# A token dispatch at the head of a loop whose 64 arms each compute the
# next token with their own arithmetic.  The table index is the token
# rebased by `lea -0x107(%r13), %eax` before a `cmp $63, %eax; ja`
# guard.  Each table entry starts one arm; the trailing zero word is no
# entry.

  .text
  .globl jt_token_loop
  .type jt_token_loop, @function
  .p2align 4
jt_token_loop:
  pushq %r13
  pushq %rbx
  movl %edi, %r13d
  movl %esi, %ebx
.Ltok_head:
  decl %ebx
  js .Ltok_done
  leal -0x107(%r13), %eax
  cmpl $63, %eax
  ja .Ltok_done
  leaq .Ltok_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ltok_c0:
  imull $3, %r13d, %r13d
  xorl $11, %r13d
  andl $1023, %r13d
  addl $263, %r13d
  jmp .Ltok_head
.Ltok_c1:
  imull $5, %r13d, %r13d
  xorl $48, %r13d
  andl $1023, %r13d
  addl $268, %r13d
  jmp .Ltok_head
.Ltok_c2:
  imull $7, %r13d, %r13d
  xorl $85, %r13d
  andl $1023, %r13d
  addl $273, %r13d
  jmp .Ltok_head
.Ltok_c3:
  imull $9, %r13d, %r13d
  xorl $122, %r13d
  andl $1023, %r13d
  addl $278, %r13d
  jmp .Ltok_head
.Ltok_c4:
  imull $11, %r13d, %r13d
  xorl $159, %r13d
  andl $1023, %r13d
  addl $283, %r13d
  jmp .Ltok_head
.Ltok_c5:
  imull $13, %r13d, %r13d
  xorl $196, %r13d
  andl $1023, %r13d
  addl $288, %r13d
  jmp .Ltok_head
.Ltok_c6:
  imull $15, %r13d, %r13d
  xorl $233, %r13d
  andl $1023, %r13d
  addl $293, %r13d
  jmp .Ltok_head
.Ltok_c7:
  imull $17, %r13d, %r13d
  xorl $270, %r13d
  andl $1023, %r13d
  addl $298, %r13d
  jmp .Ltok_head
.Ltok_c8:
  imull $19, %r13d, %r13d
  xorl $307, %r13d
  andl $1023, %r13d
  addl $303, %r13d
  jmp .Ltok_head
.Ltok_c9:
  imull $21, %r13d, %r13d
  xorl $344, %r13d
  andl $1023, %r13d
  addl $308, %r13d
  jmp .Ltok_head
.Ltok_c10:
  imull $23, %r13d, %r13d
  xorl $381, %r13d
  andl $1023, %r13d
  addl $313, %r13d
  jmp .Ltok_head
.Ltok_c11:
  imull $25, %r13d, %r13d
  xorl $418, %r13d
  andl $1023, %r13d
  addl $318, %r13d
  jmp .Ltok_head
.Ltok_c12:
  imull $27, %r13d, %r13d
  xorl $455, %r13d
  andl $1023, %r13d
  addl $323, %r13d
  jmp .Ltok_head
.Ltok_c13:
  imull $29, %r13d, %r13d
  xorl $492, %r13d
  andl $1023, %r13d
  addl $264, %r13d
  jmp .Ltok_head
.Ltok_c14:
  imull $31, %r13d, %r13d
  xorl $529, %r13d
  andl $1023, %r13d
  addl $269, %r13d
  jmp .Ltok_head
.Ltok_c15:
  imull $33, %r13d, %r13d
  xorl $566, %r13d
  andl $1023, %r13d
  addl $274, %r13d
  jmp .Ltok_head
.Ltok_c16:
  imull $35, %r13d, %r13d
  xorl $603, %r13d
  andl $1023, %r13d
  addl $279, %r13d
  jmp .Ltok_head
.Ltok_c17:
  imull $37, %r13d, %r13d
  xorl $640, %r13d
  andl $1023, %r13d
  addl $284, %r13d
  jmp .Ltok_head
.Ltok_c18:
  imull $39, %r13d, %r13d
  xorl $677, %r13d
  andl $1023, %r13d
  addl $289, %r13d
  jmp .Ltok_head
.Ltok_c19:
  imull $41, %r13d, %r13d
  xorl $714, %r13d
  andl $1023, %r13d
  addl $294, %r13d
  jmp .Ltok_head
.Ltok_c20:
  imull $43, %r13d, %r13d
  xorl $751, %r13d
  andl $1023, %r13d
  addl $299, %r13d
  jmp .Ltok_head
.Ltok_c21:
  imull $45, %r13d, %r13d
  xorl $788, %r13d
  andl $1023, %r13d
  addl $304, %r13d
  jmp .Ltok_head
.Ltok_c22:
  imull $47, %r13d, %r13d
  xorl $825, %r13d
  andl $1023, %r13d
  addl $309, %r13d
  jmp .Ltok_head
.Ltok_c23:
  imull $49, %r13d, %r13d
  xorl $862, %r13d
  andl $1023, %r13d
  addl $314, %r13d
  jmp .Ltok_head
.Ltok_c24:
  imull $51, %r13d, %r13d
  xorl $899, %r13d
  andl $1023, %r13d
  addl $319, %r13d
  jmp .Ltok_head
.Ltok_c25:
  imull $53, %r13d, %r13d
  xorl $936, %r13d
  andl $1023, %r13d
  addl $324, %r13d
  jmp .Ltok_head
.Ltok_c26:
  imull $55, %r13d, %r13d
  xorl $973, %r13d
  andl $1023, %r13d
  addl $265, %r13d
  jmp .Ltok_head
.Ltok_c27:
  imull $57, %r13d, %r13d
  xorl $1010, %r13d
  andl $1023, %r13d
  addl $270, %r13d
  jmp .Ltok_head
.Ltok_c28:
  imull $59, %r13d, %r13d
  xorl $1047, %r13d
  andl $1023, %r13d
  addl $275, %r13d
  jmp .Ltok_head
.Ltok_c29:
  imull $61, %r13d, %r13d
  xorl $1084, %r13d
  andl $1023, %r13d
  addl $280, %r13d
  jmp .Ltok_head
.Ltok_c30:
  imull $63, %r13d, %r13d
  xorl $1121, %r13d
  andl $1023, %r13d
  addl $285, %r13d
  jmp .Ltok_head
.Ltok_c31:
  imull $65, %r13d, %r13d
  xorl $1158, %r13d
  andl $1023, %r13d
  addl $290, %r13d
  jmp .Ltok_head
.Ltok_c32:
  imull $67, %r13d, %r13d
  xorl $1195, %r13d
  andl $1023, %r13d
  addl $295, %r13d
  jmp .Ltok_head
.Ltok_c33:
  imull $69, %r13d, %r13d
  xorl $1232, %r13d
  andl $1023, %r13d
  addl $300, %r13d
  jmp .Ltok_head
.Ltok_c34:
  imull $71, %r13d, %r13d
  xorl $1269, %r13d
  andl $1023, %r13d
  addl $305, %r13d
  jmp .Ltok_head
.Ltok_c35:
  imull $73, %r13d, %r13d
  xorl $1306, %r13d
  andl $1023, %r13d
  addl $310, %r13d
  jmp .Ltok_head
.Ltok_c36:
  imull $75, %r13d, %r13d
  xorl $1343, %r13d
  andl $1023, %r13d
  addl $315, %r13d
  jmp .Ltok_head
.Ltok_c37:
  imull $77, %r13d, %r13d
  xorl $1380, %r13d
  andl $1023, %r13d
  addl $320, %r13d
  jmp .Ltok_head
.Ltok_c38:
  imull $79, %r13d, %r13d
  xorl $1417, %r13d
  andl $1023, %r13d
  addl $325, %r13d
  jmp .Ltok_head
.Ltok_c39:
  imull $81, %r13d, %r13d
  xorl $1454, %r13d
  andl $1023, %r13d
  addl $266, %r13d
  jmp .Ltok_head
.Ltok_c40:
  imull $83, %r13d, %r13d
  xorl $1491, %r13d
  andl $1023, %r13d
  addl $271, %r13d
  jmp .Ltok_head
.Ltok_c41:
  imull $85, %r13d, %r13d
  xorl $1528, %r13d
  andl $1023, %r13d
  addl $276, %r13d
  jmp .Ltok_head
.Ltok_c42:
  imull $87, %r13d, %r13d
  xorl $1565, %r13d
  andl $1023, %r13d
  addl $281, %r13d
  jmp .Ltok_head
.Ltok_c43:
  imull $89, %r13d, %r13d
  xorl $1602, %r13d
  andl $1023, %r13d
  addl $286, %r13d
  jmp .Ltok_head
.Ltok_c44:
  imull $91, %r13d, %r13d
  xorl $1639, %r13d
  andl $1023, %r13d
  addl $291, %r13d
  jmp .Ltok_head
.Ltok_c45:
  imull $93, %r13d, %r13d
  xorl $1676, %r13d
  andl $1023, %r13d
  addl $296, %r13d
  jmp .Ltok_head
.Ltok_c46:
  imull $95, %r13d, %r13d
  xorl $1713, %r13d
  andl $1023, %r13d
  addl $301, %r13d
  jmp .Ltok_head
.Ltok_c47:
  imull $97, %r13d, %r13d
  xorl $1750, %r13d
  andl $1023, %r13d
  addl $306, %r13d
  jmp .Ltok_head
.Ltok_c48:
  imull $99, %r13d, %r13d
  xorl $1787, %r13d
  andl $1023, %r13d
  addl $311, %r13d
  jmp .Ltok_head
.Ltok_c49:
  imull $101, %r13d, %r13d
  xorl $1824, %r13d
  andl $1023, %r13d
  addl $316, %r13d
  jmp .Ltok_head
.Ltok_c50:
  imull $103, %r13d, %r13d
  xorl $1861, %r13d
  andl $1023, %r13d
  addl $321, %r13d
  jmp .Ltok_head
.Ltok_c51:
  imull $105, %r13d, %r13d
  xorl $1898, %r13d
  andl $1023, %r13d
  addl $326, %r13d
  jmp .Ltok_head
.Ltok_c52:
  imull $107, %r13d, %r13d
  xorl $1935, %r13d
  andl $1023, %r13d
  addl $267, %r13d
  jmp .Ltok_head
.Ltok_c53:
  imull $109, %r13d, %r13d
  xorl $1972, %r13d
  andl $1023, %r13d
  addl $272, %r13d
  jmp .Ltok_head
.Ltok_c54:
  imull $111, %r13d, %r13d
  xorl $2009, %r13d
  andl $1023, %r13d
  addl $277, %r13d
  jmp .Ltok_head
.Ltok_c55:
  imull $113, %r13d, %r13d
  xorl $2046, %r13d
  andl $1023, %r13d
  addl $282, %r13d
  jmp .Ltok_head
.Ltok_c56:
  imull $115, %r13d, %r13d
  xorl $2083, %r13d
  andl $1023, %r13d
  addl $287, %r13d
  jmp .Ltok_head
.Ltok_c57:
  imull $117, %r13d, %r13d
  xorl $2120, %r13d
  andl $1023, %r13d
  addl $292, %r13d
  jmp .Ltok_head
.Ltok_c58:
  imull $119, %r13d, %r13d
  xorl $2157, %r13d
  andl $1023, %r13d
  addl $297, %r13d
  jmp .Ltok_head
.Ltok_c59:
  imull $121, %r13d, %r13d
  xorl $2194, %r13d
  andl $1023, %r13d
  addl $302, %r13d
  jmp .Ltok_head
.Ltok_c60:
  imull $123, %r13d, %r13d
  xorl $2231, %r13d
  andl $1023, %r13d
  addl $307, %r13d
  jmp .Ltok_head
.Ltok_c61:
  imull $125, %r13d, %r13d
  xorl $2268, %r13d
  andl $1023, %r13d
  addl $312, %r13d
  jmp .Ltok_head
.Ltok_c62:
  imull $127, %r13d, %r13d
  xorl $2305, %r13d
  andl $1023, %r13d
  addl $317, %r13d
  jmp .Ltok_head
.Ltok_c63:
  imull $129, %r13d, %r13d
  xorl $2342, %r13d
  andl $1023, %r13d
  addl $322, %r13d
  jmp .Ltok_head
.Ltok_done:
  movl %r13d, %eax
  popq %rbx
  popq %r13
  ret
  .size jt_token_loop, .-jt_token_loop

  .section .rodata
  .p2align 2
.Ltok_table:
  .long .Ltok_c0-.Ltok_table
  .long .Ltok_c1-.Ltok_table
  .long .Ltok_c2-.Ltok_table
  .long .Ltok_c3-.Ltok_table
  .long .Ltok_c4-.Ltok_table
  .long .Ltok_c5-.Ltok_table
  .long .Ltok_c6-.Ltok_table
  .long .Ltok_c7-.Ltok_table
  .long .Ltok_c8-.Ltok_table
  .long .Ltok_c9-.Ltok_table
  .long .Ltok_c10-.Ltok_table
  .long .Ltok_c11-.Ltok_table
  .long .Ltok_c12-.Ltok_table
  .long .Ltok_c13-.Ltok_table
  .long .Ltok_c14-.Ltok_table
  .long .Ltok_c15-.Ltok_table
  .long .Ltok_c16-.Ltok_table
  .long .Ltok_c17-.Ltok_table
  .long .Ltok_c18-.Ltok_table
  .long .Ltok_c19-.Ltok_table
  .long .Ltok_c20-.Ltok_table
  .long .Ltok_c21-.Ltok_table
  .long .Ltok_c22-.Ltok_table
  .long .Ltok_c23-.Ltok_table
  .long .Ltok_c24-.Ltok_table
  .long .Ltok_c25-.Ltok_table
  .long .Ltok_c26-.Ltok_table
  .long .Ltok_c27-.Ltok_table
  .long .Ltok_c28-.Ltok_table
  .long .Ltok_c29-.Ltok_table
  .long .Ltok_c30-.Ltok_table
  .long .Ltok_c31-.Ltok_table
  .long .Ltok_c32-.Ltok_table
  .long .Ltok_c33-.Ltok_table
  .long .Ltok_c34-.Ltok_table
  .long .Ltok_c35-.Ltok_table
  .long .Ltok_c36-.Ltok_table
  .long .Ltok_c37-.Ltok_table
  .long .Ltok_c38-.Ltok_table
  .long .Ltok_c39-.Ltok_table
  .long .Ltok_c40-.Ltok_table
  .long .Ltok_c41-.Ltok_table
  .long .Ltok_c42-.Ltok_table
  .long .Ltok_c43-.Ltok_table
  .long .Ltok_c44-.Ltok_table
  .long .Ltok_c45-.Ltok_table
  .long .Ltok_c46-.Ltok_table
  .long .Ltok_c47-.Ltok_table
  .long .Ltok_c48-.Ltok_table
  .long .Ltok_c49-.Ltok_table
  .long .Ltok_c50-.Ltok_table
  .long .Ltok_c51-.Ltok_table
  .long .Ltok_c52-.Ltok_table
  .long .Ltok_c53-.Ltok_table
  .long .Ltok_c54-.Ltok_table
  .long .Ltok_c55-.Ltok_table
  .long .Ltok_c56-.Ltok_table
  .long .Ltok_c57-.Ltok_table
  .long .Ltok_c58-.Ltok_table
  .long .Ltok_c59-.Ltok_table
  .long .Ltok_c60-.Ltok_table
  .long .Ltok_c61-.Ltok_table
  .long .Ltok_c62-.Ltok_table
  .long .Ltok_c63-.Ltok_table
  .long 0

  .section .note.GNU-stack,"",@progbits
