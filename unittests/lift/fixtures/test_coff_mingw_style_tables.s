# Independent i386 instructions keep both dispatch shapes stable across Clang
# versions. The call table lives in executable storage; the switch table lives
# in .rdata, and its cases deliberately resemble function prologues.
.text
.globl _main, _dispatch, _classify
.p2align 4, 0xcc
_main:
  pushl $3
  calll _classify
  addl $4, %esp
  pushl %eax
  calll _dispatch
  addl $4, %esp
  retl
.p2align 4, 0xcc
_dispatch:
  movl 4(%esp), %eax
  andl $1, %eax
  pushl 4(%esp)
  calll *_Ops(,%eax,4)
  addl $4, %esp
  retl
.p2align 4, 0xcc
_classify:
  movl 4(%esp), %eax
  andl $3, %eax
  jmpl *_Cases(,%eax,4)
.Lcase0:
  pushl %ebp
  movl %esp, %ebp
  movl $11, %eax
  popl %ebp
  retl
.Lcase1:
  pushl %ebp
  movl %esp, %ebp
  movl $23, %eax
  popl %ebp
  retl
.Lcase2:
  pushl %ebp
  movl %esp, %ebp
  movl $37, %eax
  popl %ebp
  retl
.Lcase3:
  pushl %ebp
  movl %esp, %ebp
  movl $53, %eax
  popl %ebp
  retl
.p2align 4, 0xcc
_op_zero:
  movl 4(%esp), %eax
  addl $1, %eax
  retl
.p2align 4, 0xcc
_op_one:
  movl 4(%esp), %eax
  addl $2, %eax
  retl
.p2align 2, 0xcc
_Ops:
  .long _op_zero, _op_one
.section .rdata,"dr"
.p2align 2
_Cases:
  .long .Lcase0, .Lcase1, .Lcase2, .Lcase3
