.syntax unified
.arm
.text
.p2align 2

// A relocation-free table is reached first with state 1. Its two arms grow
// the domain to {1,2}; the zero sentinel exits before the normalized lookup.
// Conditional reset must preserve the predicate on the untouched old state.
.macro state_domain name, compared, sentinel
.globl \name
.type \name,%function
\name:
  push {r4,lr}
  mov r3,r1
  mov r2,#0
  mov r1,#1
  b .Ldispatch\@
.Lfirst\@:
  add r2,r2,#1
  mov r1,#2
  b .Ldispatch\@
.Lsecond\@:
  and r1,r0,#0x70
  cmp \compared,#0
  movne r1,#1
  b .Ldispatch\@
.Ldispatch\@:
  cmp r2,#8
  beq .Ldone\@
  cmp r1,#\sentinel
  beq .Ldone\@
  sub r1,r1,#1
  adr r4,.Ltable\@
  ldr r1,[r4,r1,lsl #2]
  add pc,r4,r1
.Ltable\@:
  .word .Lfirst\@-.Ltable\@
  .word .Lsecond\@-.Ltable\@
.Ldone\@:
  mov r0,r2
  pop {r4,pc}
.size \name,.-\name
.endm

state_domain arm_inline_state_domain,r1,0
// r3 is an independent incoming argument: its condition cannot constrain r1.
state_domain arm_inline_state_other_condition,r3,0
// State zero now reaches state-1. Never publish the otherwise valid prefix.
state_domain arm_inline_state_wrong_sentinel,r1,3

// State 2 selects between two constants. Subtracting one at the dispatch
// must account for both incoming arms, including a possible out-of-range one.
.macro merged_state name, next
.globl \name
.type \name,%function
\name:
  push {r4,lr}
  mov r1,#1
  mov r2,#0
  b .Ldispatch\@
.Ladvance\@:
  add r2,r2,#1
  cmp r1,#0
  beq .Ldone\@
.Ldispatch\@:
  cmp r2,#8
  beq .Ldone\@
  sub r1,r1,#1
  adr r4,.Ltable\@
  ldr r1,[r4,r1,lsl #2]
  add pc,r4,r1
.Ltable\@:
  .word .Lfirst\@-.Ltable\@
  .word .Lsecond\@-.Ltable\@
  .word .Lthird\@-.Ltable\@
  .word .Lfourth\@-.Ltable\@
.Lfirst\@:
  mov r1,#2
  b .Ladvance\@
.Lsecond\@:
  tst r0,#1
  mov r1,#3
  movweq r1,#\next
  b .Ladvance\@
.Lthird\@:
  mov r1,#1
  b .Ladvance\@
.Lfourth\@:
  mov r1,#0
  b .Ladvance\@
.Ldone\@:
  mov r0,r2
  pop {r4,pc}
.size \name,.-\name
.endm

merged_state arm_inline_merged_states,4
merged_state arm_inline_merged_outside,9
