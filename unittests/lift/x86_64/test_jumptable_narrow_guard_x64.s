// Long upstream arithmetic must not obscure the exact register lane checked
// by the guard. The fourth physical slot is outside the admitted domain.
.text
.macro NARROW_GUARD name, mode=0
.globl \name
.type \name,@function
\name:
  movl %edi, %edx
  .rept 100
  addl $1, %edx
  .endr
  leal -76(%edx), %eax
  .if \mode == 1
  cmpb $2, %ah
  .elseif \mode == 2
  cmpw $2, %ax
  .else
  cmpb $2, %al
  .endif
  ja .Ldefault\@
  .if \mode == 3
  movb %cl, %al
  .elseif \mode == 4
  movl %ecx, %eax
  .elseif \mode == 7
  movb %cl, %ah
  .elseif \mode == 8
  call narrow_guard_barrier
  .endif
  .if \mode == 1 || \mode == 5
  movzbl %ah, %eax
  .elseif \mode == 2
  movzwl %ax, %eax
  .elseif \mode != 6
  movzbl %al, %eax
  .endif
  leaq .Ltable\@(%rip), %rdx
  movslq (%rdx,%rax,4), %rax
  addq %rdx,%rax
  jmp *%rax
.Lcase0\@: movl $710, %eax
  ret
.Lcase1\@: movl $711, %eax
  ret
.Lcase2\@: movl $712, %eax
  ret
.Lpoison\@: movl $799, %eax
  ret
.Ldefault\@:
  .if \mode == 9
  .rept 200
  addl $1, %edx
  .endr
  movl %edx, %eax
  .else
  movl $798, %eax
  .endif
  ret
.size \name,.-\name
.pushsection .rodata,"a",@progbits
.p2align 2
.Ltable\@:
  .long .Lcase0\@-.Ltable\@,.Lcase1\@-.Ltable\@,.Lcase2\@-.Ltable\@,.Lpoison\@-.Ltable\@
.popsection
.endm
NARROW_GUARD narrow_guard_low
NARROW_GUARD narrow_guard_high, 1
NARROW_GUARD narrow_guard_word, 2
NARROW_GUARD narrow_guard_changed_low, 3
NARROW_GUARD narrow_guard_changed_full, 4
NARROW_GUARD narrow_guard_other_lane, 5
NARROW_GUARD narrow_guard_unextended, 6
NARROW_GUARD narrow_guard_disjoint_write, 7
NARROW_GUARD narrow_guard_call, 8
NARROW_GUARD narrow_guard_scalar_return, 9
.globl narrow_guard_barrier
.type narrow_guard_barrier,@function
narrow_guard_barrier:
  ret
.size narrow_guard_barrier,.-narrow_guard_barrier
