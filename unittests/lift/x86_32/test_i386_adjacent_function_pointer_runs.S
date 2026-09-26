// Four directly reachable GOT-relative tail dispatches over two contiguous
// five-entry function-pointer arrays. Each array has two exact consumers.
// The arrays must remain tail calls even though their indexed LOAD shape and
// raw R_386_GOTOFF fields match the finite adjacent jump-table group.

  .text
  .globl x86tt_fourtails
  .type x86tt_fourtails, @function
x86tt_fourtails:
  call .Lgot_seed
.Lgot_seed:
  popl %edx
  addl $_GLOBAL_OFFSET_TABLE_+(.-.Lgot_seed), %edx
  movl 4(%esp), %eax
  cmpl $0, %eax
  je .Ltail_a0
  cmpl $1, %eax
  je .Ltail_a1
  cmpl $2, %eax
  je .Ltail_b0
  jmp .Ltail_b1
.Ltail_a0:
  xorl %ecx, %ecx
  movl .Larray_a@GOTOFF(%edx,%ecx,4), %ecx
  addl %edx, %ecx
  jmp *%ecx
.Ltail_a1:
  xorl %ecx, %ecx
  movl .Larray_a@GOTOFF(%edx,%ecx,4), %ecx
  addl %edx, %ecx
  jmp *%ecx
.Ltail_b0:
  xorl %ecx, %ecx
  movl .Larray_b@GOTOFF(%edx,%ecx,4), %ecx
  addl %edx, %ecx
  jmp *%ecx
.Ltail_b1:
  xorl %ecx, %ecx
  movl .Larray_b@GOTOFF(%edx,%ecx,4), %ecx
  addl %edx, %ecx
  jmp *%ecx
  .size x86tt_fourtails, .-x86tt_fourtails

  .globl x86tt_tail_target
  .type x86tt_tail_target, @function
x86tt_tail_target:
  movl $7, %eax
  ret
  .size x86tt_tail_target, .-x86tt_tail_target

  .section .rodata, "a", @progbits
.Larray_a:
  .rept 5
  .long x86tt_tail_target@GOTOFF
  .endr
.Larray_b:
  .rept 5
  .long x86tt_tail_target@GOTOFF
  .endr
