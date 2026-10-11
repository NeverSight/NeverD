// W0 is a lane of the last X0 definition; the upper word remains unbounded.
.text
.macro NARROW_GUARD name, mode=0
.globl \name
.type \name,@function
\name:
  .rept 100
  add x0, x0, #1
  .endr
  cmp w0, #2
  b.hi .Ldefault\@
  .if \mode == 1
  mov w0, w1
  .elseif \mode == 2
  mov x0, x1
  .endif
  .if \mode != 3
  mov w0, w0
  .endif
  adrp x2, .Ltable\@
  add x2, x2, :lo12:.Ltable\@
  ldrsw x3, [x2,x0,lsl #2]
  add x3, x2, x3
  br x3
.Lcase0\@: mov w0, #710
  ret
.Lcase1\@: mov w0, #711
  ret
.Lcase2\@: mov w0, #712
  ret
.Lpoison\@: mov w0, #799
  ret
.Ldefault\@: mov w0, #798
  ret
.size \name,.-\name
.pushsection .rodata,"a",@progbits
.p2align 2
.Ltable\@:
  .word .Lcase0\@-.Ltable\@,.Lcase1\@-.Ltable\@,.Lcase2\@-.Ltable\@,.Lpoison\@-.Ltable\@
.popsection
.endm
NARROW_GUARD narrow_guard_low
NARROW_GUARD narrow_guard_changed_low, 1
NARROW_GUARD narrow_guard_changed_full, 2
NARROW_GUARD narrow_guard_unextended, 3
