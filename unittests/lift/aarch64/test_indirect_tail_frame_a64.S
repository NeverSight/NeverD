// Independently constructed native tail-transfer and live-frame controls.
.arch armv8.1-a
.text
.macro BEGIN name
.p2align 2
.globl \name
.type \name,@function
\name:
.endm
.macro END name
.size \name,.-\name
.endm

BEGIN tail_frame_live
  sub sp,sp,#64
  br x0
END tail_frame_live

BEGIN tail_frame_unknown
  mov sp,x1
  br x0
END tail_frame_unknown

BEGIN tail_frame_join
  sub sp,sp,#32
  cbz w1,.Ljoined
  add sp,sp,#32
.Ljoined:
  br x0
END tail_frame_join

BEGIN tail_link_changed
  mov x30,x1
  br x0
END tail_link_changed

BEGIN tail_link_unrestored
  stp x29,x30,[sp,#-16]!
  bl tail_helper
  add sp,sp,#16
  br x0
END tail_link_unrestored

BEGIN tail_link_slot_changed
  stp x29,x30,[sp,#-16]!
  str x1,[sp,#8]
  ldp x29,x30,[sp],#16
  br x0
END tail_link_slot_changed

BEGIN tail_link_slot_partial
  stp x29,x30,[sp,#-16]!
  strb w1,[sp,#8]
  ldp x29,x30,[sp],#16
  br x0
END tail_link_slot_partial

BEGIN tail_unknown_alias
  stp x29,x30,[sp,#-16]!
  str x1,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_unknown_alias

BEGIN tail_frame_borrowed
  stp x29,x30,[sp,#-16]!
  mov x0,sp
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x1
END tail_frame_borrowed

BEGIN tail_stack_target
  mov x0,sp
  br x0
END tail_stack_target

BEGIN tail_link_target
  mov x0,x30
  br x0
END tail_link_target

BEGIN tail_frame_loop
.Lframe_loop:
  cbz w1,.Lframe_exit
  sub sp,sp,#16
  b .Lframe_loop
.Lframe_exit:
  br x0
END tail_frame_loop

BEGIN tail_link_loop
.Llink_loop:
  cbz w1,.Llink_exit
  mov x30,x2
  b .Llink_loop
.Llink_exit:
  br x0
END tail_link_loop

BEGIN tail_independent_root
  b .Lroot_join
.Laddress_root:
  add sp,sp,#16
.Lroot_join:
  br x0
END tail_independent_root

BEGIN tail_leaf
  br x0
END tail_leaf

BEGIN tail_frame_balanced
  stp x29,x30,[sp,#-16]!
  mov x29,sp
  sub sp,sp,#64
  add sp,sp,#64
  ldp x29,x30,[sp],#16
  br x0
END tail_frame_balanced

BEGIN tail_frame_diamond
  cbz w1,.Lshort
  sub sp,sp,#32
  add sp,sp,#32
  b .Ldone
.Lshort:
  sub sp,sp,#16
  add sp,sp,#16
.Ldone:
  br x0
END tail_frame_diamond

BEGIN tail_loop_balanced
.Lbalanced_loop:
  cbz w1,.Lbalanced_exit
  sub sp,sp,#16
  add sp,sp,#16
  b .Lbalanced_loop
.Lbalanced_exit:
  br x0
END tail_loop_balanced

BEGIN tail_link_restored
  stp x29,x30,[sp,#-16]!
  mov x29,sp
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_link_restored

// libc++'s __shared_weak_count::__release_weak: an acquire load and an
// exclusive decrement loop before the virtual tail call.
BEGIN tail_exclusive_release
  add x8,x0,#16
  ldar x9,[x8]
  cbz x9,.Lrelease_call
.Lrelease_retry:
  ldaxr x9,[x8]
  sub x10,x9,#1
  stlxr w11,x10,[x8]
  cbnz w11,.Lrelease_retry
  cbz x9,.Lrelease_call
  ret
.Lrelease_call:
  ldr x8,[x0]
  ldr x1,[x8,#32]
  br x1
END tail_exclusive_release

BEGIN tail_exclusive_pair
.Lpair_retry:
  ldaxp x2,x3,[x1]
  add x2,x2,#1
  stlxp w4,x2,x3,[x1]
  cbnz w4,.Lpair_retry
  br x0
END tail_exclusive_pair

BEGIN tail_lse_atomics
  ldaddal x2,x3,[x1]
  swpal x2,x3,[x1]
  casal x2,x3,[x1]
  ldclral x2,x3,[x1]
  stadd x2,[x1]
  br x0
END tail_lse_atomics

BEGIN tail_value_intrinsic
  rbit x1,x1
  br x0
END tail_value_intrinsic

BEGIN tail_exclusive_live
  sub sp,sp,#16
.Llive_retry:
  ldaxr x2,[x1]
  stlxr w3,x2,[x1]
  cbnz w3,.Llive_retry
  br x0
END tail_exclusive_live

// An exclusive store or an atomic swap may overwrite the saved link word.
BEGIN tail_exclusive_link_slot
  stp x29,x30,[sp,#-16]!
  add x2,sp,#8
.Llink_slot_retry:
  ldaxr x3,[x2]
  stlxr w4,x1,[x2]
  cbnz w4,.Llink_slot_retry
  ldp x29,x30,[sp],#16
  br x0
END tail_exclusive_link_slot

BEGIN tail_atomic_link_slot
  stp x29,x30,[sp,#-16]!
  add x2,sp,#8
  swpal x1,x3,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_atomic_link_slot

// A frame address published through an exclusive store or an atomic swap
// lets a later callee rewrite the saved link word, though no argument
// register carries it.
BEGIN tail_exclusive_escape
  mov x9,sp
.Lescape_retry:
  ldaxr x10,[x2]
  stlxr w11,x9,[x2]
  cbnz w11,.Lescape_retry
  stp x29,x30,[sp,#-16]!
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_exclusive_escape

BEGIN tail_atomic_escape
  mov x9,sp
  swpal x9,x10,[x2]
  stp x29,x30,[sp,#-16]!
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_atomic_escape

BEGIN tail_exclusive_link_load
  ldaxr x30,[x1]
  br x0
END tail_exclusive_link_load

// A supervisor call has no explicit effect contract.
BEGIN tail_unmodelled_intrinsic
  svc #0
  br x0
END tail_unmodelled_intrinsic

// libc++'s __shared_weak_count::__release_shared: a call, then the exclusive
// decrement of the object's count while the link word is saved.  A store
// through a pointer the function did not derive from its frame cannot reach
// that frame.
BEGIN tail_saved_link_foreign_store
  stp x20,x19,[sp,#-32]!
  stp x29,x30,[sp,#16]
  add x29,sp,#16
  mov x19,x0
  bl tail_helper
  add x8,x19,#8
.Lforeign_retry:
  ldaxr x9,[x8]
  sub x10,x9,#1
  stlxr w11,x10,[x8]
  cbnz w11,.Lforeign_retry
  ldr x8,[x19]
  ldp x29,x30,[sp,#16]
  mov x0,x19
  ldr x1,[x8,#32]
  ldp x20,x19,[sp],#32
  br x1
END tail_saved_link_foreign_store

// A store through an address derived from the frame may hit the saved link
// word.
BEGIN tail_frame_alias
  stp x29,x30,[sp,#-16]!
  add x2,sp,x3
  str x1,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_frame_alias

// Once a frame address has left for memory the function does not track, any
// store may be the one through it.
BEGIN tail_exposed_frame_alias
  stp x29,x30,[sp,#-16]!
  mov x9,sp
  str x9,[x3]
  str x1,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_exposed_frame_alias

// A frame address spilled to the frame and read back through an address the
// spills cannot follow is still a frame address.
BEGIN tail_reloaded_frame_alias
  stp x29,x30,[sp,#-32]!
  add x9,sp,#8
  str x9,[sp,#16]
  ldr x10,[sp,x4]
  str x1,[x10]
  ldp x29,x30,[sp],#32
  br x0
END tail_reloaded_frame_alias

// Half of a frame address stored where no spill records it, or a frame
// address stored above the frame, escapes as surely as one stored elsewhere.
BEGIN tail_narrow_frame_alias
  stp x29,x30,[sp,#-32]!
  add x9,sp,#8
  str w9,[sp,#16]
  str x1,[x2]
  ldp x29,x30,[sp],#32
  br x0
END tail_narrow_frame_alias

BEGIN tail_outside_frame_alias
  stp x29,x30,[sp,#-16]!
  mov x9,sp
  str x9,[sp,#24]
  str x1,[x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_outside_frame_alias

// A trap ends its own path; the other path still restores the frame.
BEGIN tail_after_trap_path
  stp x29,x30,[sp,#-16]!
  cbz x1,.Ltrap_path
  ldp x29,x30,[sp],#16
  br x0
.Ltrap_path:
  brk #0x3e8
END tail_after_trap_path

BEGIN tail_udf_padding
  br x0
  udf #0
END tail_udf_padding

// The thread pointer read writes only its destination.
BEGIN tail_thread_pointer
  mrs x1,tpidr_el0
  str x2,[x1,#8]
  br x0
END tail_thread_pointer

BEGIN tail_link_from_sysreg
  mrs x30,tpidr_el0
  br x0
END tail_link_from_sysreg

// Indexed accesses retain their original address instruction in LowIR. They
// change memory/the load destination, not the incoming frame or link.
BEGIN tail_indexed_leaf
  ldr x3,[x1,x2,lsl #3]
  br x0
END tail_indexed_leaf

BEGIN tail_indexed_signed
  ldr x3,[x1,w2,sxtw #3]
  br x0
END tail_indexed_signed

BEGIN tail_indexed_virtual
  ldr x9,[x0,#8]
  ldr x0,[x9,x1,lsl #3]
  ldr x10,[x0]
  ldr x16,[x10,#40]
  br x16
END tail_indexed_virtual

BEGIN tail_indexed_virtual_zero
  ldr x9,[x0,#8]
  ldr x0,[x9,x1,lsl #3]
  cbz x0,.Lindexed_virtual_null
  ldr x10,[x0]
  ldr x16,[x10]
  br x16
.Lindexed_virtual_null:
  ret
END tail_indexed_virtual_zero

// A direct load becomes a propagated temporary in MedIR; X0 still carries
// that same receiver when the guarded tail call reaches slot zero.
BEGIN tail_direct_virtual_zero
  ldr x0,[x0,#8]
  cbz x0,.Ldirect_virtual_null
  ldr x10,[x0]
  ldr x16,[x10]
  br x16
.Ldirect_virtual_null:
  ret
END tail_direct_virtual_zero

BEGIN tail_indexed_foreign_store
  stp x29,x30,[sp,#-16]!
  str x3,[x1,x2]
  ldp x29,x30,[sp],#16
  br x0
END tail_indexed_foreign_store

BEGIN tail_indexed_vector
  ldr q0,[x1,x2]
  str q0,[x1,x2]
  br x0
END tail_indexed_vector

BEGIN tail_indexed_after_call
  stp x29,x30,[sp,#-16]!
  strb w3,[x1,x2]
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_indexed_after_call

// Either address operand may carry a frame alias. An indexed store cannot
// preserve the saved link when its address may name that slot.
BEGIN tail_indexed_link_slot
  stp x29,x30,[sp,#-16]!
  mov x8,sp
  mov x9,#8
  str x2,[x8,x9]
  ldp x29,x30,[sp],#16
  br x0
END tail_indexed_link_slot

BEGIN tail_indexed_frame_index
  stp x29,x30,[sp,#-16]!
  add x9,sp,#8
  mov x8,#0
  str x2,[x8,x9]
  ldp x29,x30,[sp],#16
  br x0
END tail_indexed_frame_index

BEGIN tail_indexed_frame_escape
  stp x29,x30,[sp,#-16]!
  mov x3,sp
  str x3,[x1,x2]
  bl tail_helper
  ldp x29,x30,[sp],#16
  br x0
END tail_indexed_frame_escape

BEGIN tail_indexed_link_load
  ldr x30,[x1,x2]
  br x0
END tail_indexed_link_load

BEGIN tail_indexed_stack_target
  stp x29,x30,[sp,#-32]!
  add x9,sp,#8
  str x9,[sp,#16]
  mov x8,sp
  mov x9,#16
  ldr x0,[x8,x9]
  ldp x29,x30,[sp],#32
  br x0
END tail_indexed_stack_target

BEGIN tail_indexed_local_target
  stp x29,x30,[sp,#-32]!
  adr x9,.Lindexed_local
  str x9,[sp,#16]
  mov x8,sp
  mov x9,#16
  ldr x0,[x8,x9]
  ldp x29,x30,[sp],#32
  br x0
.Lindexed_local:
  mov x0,#17
  ret
END tail_indexed_local_target

BEGIN tail_helper
  ret
END tail_helper
.data
.p2align 3
.quad .Laddress_root
.section .note.GNU-stack,"",@progbits
