.text

// In i386, ESP itself is the architectural full-width stack pointer.  A
// 32-bit self-copy must therefore preserve the private-frame epoch rather than
// being treated like the x86-64 ESP-to-RSP zero-extension rule.
.p2align 2
.globl jt_i386_private_frame_spill
.type jt_i386_private_frame_spill, @function
jt_i386_private_frame_spill:
  movl 4(%esp), %eax
  cmpl $1, %eax
  ja .Ljt_i386_private_default
  subl $4, %esp
  movl %esp, %esp
  movl $.Ljt_i386_private_table, (%esp)
  movl (%esp), %edx
  andl $1, %eax
  jmp *.Ljt_i386_private_table(,%eax,4)
.Ljt_i386_private_case0:
  addl $4, %esp
  movl $3200, %eax
  ret
.Ljt_i386_private_case1:
  addl $4, %esp
  movl $3201, %eax
  ret
.Ljt_i386_private_default:
  movl $-1, %eax
  ret
.size jt_i386_private_frame_spill, .-jt_i386_private_frame_spill

.section .data.jt_i386_private,"aw",@progbits
.p2align 2
.Ljt_i386_private_table:
  .long .Ljt_i386_private_case0
  .long .Ljt_i386_private_case1
