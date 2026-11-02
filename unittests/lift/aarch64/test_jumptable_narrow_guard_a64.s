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

// Neither arm's range check dominates the joined compact table. Both must
// bound the exact selector, including the implicit zero extension of W8.
.macro JOINED_GUARD name, mode=0
.globl \name
.type \name,@function
\name:
  cbz x1, .Lother\@
  cmp w0, #2
  b.hi .Ldefault\@
  b .Ljoin\@
.Lother\@:
  .if \mode != 1
  cmp w0, #2
  b.hi .Ldefault\@
  .endif
  .if \mode == 2
  mov w0, w2
  .endif
.Ljoin\@:
  .if \mode == 3
  mov x8, x0
  .else
  mov w8, w0
  .endif
  adrp x9, .Ltable\@
  add x9, x9, :lo12:.Ltable\@
  adr x10, .Lcase0\@
  ldrb w11, [x9, x8]
  add x10, x10, x11, lsl #2
  br x10
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
.Ltable\@:
  .byte 0, (.Lcase1\@-.Lcase0\@)/4, (.Lcase2\@-.Lcase0\@)/4, (.Lpoison\@-.Lcase0\@)/4
.popsection
.endm
JOINED_GUARD joined_guard
JOINED_GUARD joined_guard_missing, 1
JOINED_GUARD joined_guard_changed, 2
JOINED_GUARD joined_guard_unextended, 3
