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

// `ubfx x` clears every bit above its field. A following `cmp w` of that
// register therefore bounds the full X index. A field that still has a bit
// above the W lane does not, and neither does a later write of the register.
.macro UBFX_WCMP name, kind=0
.globl \name
.type \name,@function
\name:
  .if \kind == 1
  ubfx x0, x0, #8, #40
  .else
  ubfx x0, x0, #16, #16
  .endif
  cmp w0, #2
  b.hi .Ldefault\@
  .if \kind == 2
  mov w0, w1
  .endif
  adrp x2, .Ltable\@
  add x2, x2, :lo12:.Ltable\@
  ldrsw x3, [x2, x0, lsl #2]
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
UBFX_WCMP ubfx_wcmp_low
UBFX_WCMP ubfx_wcmp_wide, 1
UBFX_WCMP ubfx_wcmp_replaced, 2

// The bitfield admits more values than the compare. The executable table
// continues through that wider envelope; the extra words are zero and decode
// as the adr anchor. The compare is the selector domain.
.macro GUARDED_ADR_MASK name, kind=0
.globl \name
.type \name,@function
\name:
  ubfx x8, x1, #26, #3
  cmp x8, #2
  b.hi .Ldefault\@
  .if \kind == 1
  mov x8, x12
  .endif
  adrp x9, .Ltable\@
  add x9, x9, :lo12:.Ltable\@
  adr x10, .Lanchor\@
.Lanchor\@:
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  br x10
.Lcase0\@: mov w0, #710
  ret
.Lcase1\@: mov w0, #711
  ret
.Lcase2\@: mov w0, #712
  ret
.Ldefault\@: mov w0, #798
  ret
.size \name,.-\name
.pushsection .guard_adr_table,"ax",@progbits
.p2align 2
.globl \name\()_table
\name\()_table:
.Ltable\@:
  .word .Lcase0\@-.Lanchor\@
  .word .Lcase1\@-.Lanchor\@
  .word .Lcase2\@-.Lanchor\@
  .zero 20
.size \name\()_table, .-\name\()_table
.popsection
.endm
GUARDED_ADR_MASK guarded_adr_mask_continues
GUARDED_ADR_MASK guarded_adr_mask_replaced, 1

// An earlier equality exit removes one value from an unsigned range. The
// table still has that slot. kind 1 excludes two adjacent values. kind 2 has
// no range compare. byte=1 is the compact byte table.
.macro EQUALITY_HOLE name, byte=0, kind=0
.globl \name
.type \name,@function
\name:
  .if \kind == 1
  cmp w0, #1
  b.eq .Ldef\@
  cmp w0, #2
  b.eq .Ldef\@
  cmp w0, #4
  b.hi .Ldef\@
  .elseif \kind == 2
  cmp w0, #1
  b.eq .Ldef\@
  cmp w0, #3
  b.eq .Ldef\@
  .else
  cmp w0, #1
  b.eq .Ldef\@
  cmp w0, #3
  b.hi .Ldef\@
  .endif
  adrp x9, .Ltable\@
  mov w8, w0
  add x9, x9, :lo12:.Ltable\@
  adr x10, .Lanchor\@
.Lanchor\@:
  .if \byte
  ldrb w11, [x9, x8]
  add x10, x10, x11, lsl #2
  .else
  ldrsw x11, [x9, x8, lsl #2]
  add x10, x10, x11
  .endif
  br x10
.Ldef\@:
  mov w0, #790
  ret
.Lc0\@:
  mov w0, #700
  ret
.Lc2\@:
  mov w0, #702
  ret
.Lc3\@:
  mov w0, #703
  ret
.Lc4\@:
  mov w0, #704
  ret
.size \name, .-\name
.pushsection .rodata,"a",@progbits
.p2align 2
.Ltable\@:
  .if \byte
  .byte (.Lc0\@-.Lanchor\@)/4
  .byte (.Ldef\@-.Lanchor\@)/4
  .byte (.Lc2\@-.Lanchor\@)/4
  .byte (.Lc3\@-.Lanchor\@)/4
  .byte (.Lc4\@-.Lanchor\@)/4
  .else
  .word .Lc0\@-.Lanchor\@
  .word .Ldef\@-.Lanchor\@
  .word .Lc2\@-.Lanchor\@
  .word .Lc3\@-.Lanchor\@
  .word .Lc4\@-.Lanchor\@
  .endif
.popsection
.endm
EQUALITY_HOLE equality_hole_word
EQUALITY_HOLE equality_hole_byte, 1
EQUALITY_HOLE equality_hole_adjacent, 0, 1
EQUALITY_HOLE equality_hole_unbounded, 0, 2

// A dominating equality is not a dense prefix. The predecessor ranges do not
// dominate the load; the joined proof still has to bound every incoming path.
// kind 1 leaves one path unchecked. kind 2 is a tight dominating range plus a
// wider compare of another register. kind 3 keeps both checks dominating.
// kind 4 has no dominating index guard, only the two ranges and that wider
// compare, so the literal envelope is not the published bound.
// kind 5 is a dominating signed compare wider than the two unsigned
// predecessor ranges. kind 6 drops one of those ranges. kind 7 keeps the
// signed compare and the unsigned range on one dominating path. Bytes past
// the three real slots are not valid cases.
.globl joined_guard_leaf
.type joined_guard_leaf,@function
joined_guard_leaf:
  mov w0, #1
  ret
.size joined_guard_leaf, .-joined_guard_leaf

.macro JOINED_AFTER_EQUALITY name, kind=0
.globl \name
.type \name,@function
\name:
  .if \kind == 2
  cmp w0, #2
  b.hi .Ldefault\@
  cmp w2, #20
  b.hi .Ldefault\@
  mov w8, w0
  .elseif \kind == 3
  cmp w0, #4
  b.eq .Ldefault\@
  cmp w0, #2
  b.hi .Ldefault\@
  mov w8, w0
  .elseif \kind == 4
  cmp w2, #20
  b.hi .Ldefault\@
  cbz x1, .Lother\@
  cmp w0, #2
  b.hi .Ldefault\@
  b .Ljoin\@
.Lother\@:
  cmp w0, #2
  b.hi .Ldefault\@
.Ljoin\@:
  mov w8, w0
  .elseif \kind == 5
  cmp w0, #6
  b.gt .Ldefault\@
  cbz x1, .Lother\@
  cmp w0, #2
  b.hi .Ldefault\@
  b .Ljoin\@
.Lother\@:
  cmp w0, #2
  b.hi .Ldefault\@
.Ljoin\@:
  mov w8, w0
  .elseif \kind == 6
  cmp w0, #6
  b.gt .Ldefault\@
  cbz x1, .Ljoin\@
  cmp w0, #2
  b.hi .Ldefault\@
  b .Ljoin\@
.Ljoin\@:
  mov w8, w0
  .elseif \kind == 7
  cmp w0, #6
  b.gt .Ldefault\@
  cmp w0, #2
  b.hi .Ldefault\@
  mov w8, w0
  .else
  cmp w0, #4
  b.eq .Ldefault\@
  mov w21, w0
  .if \kind == 1
  cbz x1, .Ljoin\@
  .else
  cbz x1, .Lcall\@
  .endif
  cmp w21, #2
  b.hi .Ldefault\@
  b .Ljoin\@
  .if \kind != 1
.Lcall\@:
  bl joined_guard_leaf
  cmp w21, #2
  b.hi .Ldefault\@
  .endif
.Ljoin\@:
  mov w8, w21
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
  .if \kind >= 5
  .byte 0xff, 0xff, 0xff
  .endif
.popsection
.endm
JOINED_AFTER_EQUALITY joined_after_equality
JOINED_AFTER_EQUALITY joined_after_equality_missing, 1
JOINED_AFTER_EQUALITY joined_after_equality_tight, 2
JOINED_AFTER_EQUALITY joined_after_equality_dominating, 3
JOINED_AFTER_EQUALITY joined_after_equality_wide_literal, 4
JOINED_AFTER_EQUALITY joined_after_signed, 5
JOINED_AFTER_EQUALITY joined_after_signed_missing, 6
JOINED_AFTER_EQUALITY joined_after_signed_dominating, 7
