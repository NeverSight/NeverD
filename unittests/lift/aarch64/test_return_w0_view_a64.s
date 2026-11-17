// A compare of w0 is not a write of x0. The call's result still occupies the
// whole register on both sides of the branch, and a compare of the incoming
// argument still returns that argument. A 32-bit write defines the zeros it
// produces; those are not the high bits of some earlier value.

        .text
        .globl  w0_view_callee
        .type   w0_view_callee,%function
// Returns a computed x0. A bare ret would leave the caller's x0 in place,
// and these tests are about the value this call returns.
w0_view_callee:
        add     x0, x0, #1
        ret
        .size   w0_view_callee, .-w0_view_callee

        .globl  call_result_survives_w0_test
        .type   call_result_survives_w0_test,%function
call_result_survives_w0_test:
        stp     x29, x30, [sp, #-32]!
        str     x19, [sp, #16]
        mov     x29, sp
        mov     x19, x0
        mov     x1, xzr
        mov     x2, xzr
        bl      w0_view_callee
        cbnz    w0, .Lcall_skip
        ldr     w8, [x19, #4]
        str     w8, [x19, #12]
.Lcall_skip:
        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
        .size   call_result_survives_w0_test, .-call_result_survives_w0_test

// One path writes w0. The other only compares it, then returns the argument
// that arrived in x0. The compare must not narrow that argument.
        .globl  w0_compare_returns_full_argument
        .type   w0_compare_returns_full_argument,%function
w0_compare_returns_full_argument:
        cbz     w2, .Larg_bare
        mov     w0, #7
        ret
.Larg_bare:
        cbnz    w0, .Larg_keep
        str     wzr, [x1]
.Larg_keep:
        ret
        .size   w0_compare_returns_full_argument, .-w0_compare_returns_full_argument

// The compare's join is not the block that returns. A later branch splits
// the epilogue and does not write x0, so the call result is still there.
        .globl  call_result_survives_a_later_branch
        .type   call_result_survives_a_later_branch,%function
call_result_survives_a_later_branch:
        stp     x29, x30, [sp, #-32]!
        str     x19, [sp, #16]
        mov     x29, sp
        mov     x19, x0
        bl      w0_view_callee
        cbnz    w0, .Llater_skip
        str     wzr, [x29, #24]
.Llater_skip:
        cbz     x19, .Llater_const
        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
.Llater_const:
        mov     x0, #7
        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
        .size   call_result_survives_a_later_branch, .-call_result_survives_a_later_branch

// One side overwrites x0 before the later branch. The return is that join.
        .globl  later_branch_keeps_an_overwriting_side
        .type   later_branch_keeps_an_overwriting_side,%function
later_branch_keeps_an_overwriting_side:
        stp     x29, x30, [sp, #-48]!
        stp     x19, x20, [sp, #16]
        mov     x29, sp
        mov     x19, x1
        mov     x20, x2
        bl      w0_view_callee
        cbz     x19, .Lover_keep
        mov     x0, #4
.Lover_keep:
        cbz     x20, .Lover_seven
        ldp     x19, x20, [sp, #16]
        ldp     x29, x30, [sp], #48
        ret
.Lover_seven:
        mov     x0, #7
        ldp     x19, x20, [sp, #16]
        ldp     x29, x30, [sp], #48
        ret
        .size   later_branch_keeps_an_overwriting_side, .-later_branch_keeps_an_overwriting_side

// mov w0, #7 writes the low half and zeros the high half. The result is 7.
        .globl  w_write_defines_the_result
        .type   w_write_defines_the_result,%function
w_write_defines_the_result:
        mov     w0, #7
        ret
        .size   w_write_defines_the_result, .-w_write_defines_the_result

// The call writes x0. The other side does not, so that side still holds the
// argument. A load on the other side is that load, not the argument.
        .globl  return_nine
        .type   return_nine,%function
return_nine:
        mov     x0, #9
        ret
        .size   return_nine, .-return_nine

        .globl  argument_survives_beside_a_call
        .type   argument_survives_beside_a_call,%function
argument_survives_beside_a_call:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        cmp     w1, #1
        b.ne    .Lkeep_arg
        bl      return_nine
.Lkeep_arg:
        ldp     x29, x30, [sp], #16
        ret
        .size   argument_survives_beside_a_call, .-argument_survives_beside_a_call

        .globl  read_argument_survives
        .type   read_argument_survives,%function
read_argument_survives:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      argument_survives_beside_a_call
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_argument_survives, .-read_argument_survives

// The 32-bit argument is saved, then a call may replace x0. The null edge
// never writes x0. The pointer used after the call is callee-saved.
        .globl  saved_argument_beside_call
        .type   saved_argument_beside_call,%function
saved_argument_beside_call:
        stp     x29, x30, [sp, #-32]!
        stp     x19, x20, [sp, #16]
        mov     x29, sp
        mov     w19, w0
        mov     x20, x2
        cbz     x1, .Lsaved_skip
        mov     x0, x1
        bl      return_nine
.Lsaved_skip:
        str     w19, [x20]
        ldp     x19, x20, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
        .size   saved_argument_beside_call, .-saved_argument_beside_call

        .globl  read_saved_argument
        .type   read_saved_argument,%function
read_saved_argument:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      saved_argument_beside_call
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_saved_argument, .-read_saved_argument

// One edge writes w0. The other edge does not, so it still holds the argument.
        .globl  w_write_beside_argument
        .type   w_write_beside_argument,%function
w_write_beside_argument:
        cmp     w1, #1
        b.ne    .Lwwrite_skip
        mov     w0, #7
.Lwwrite_skip:
        ret
        .size   w_write_beside_argument, .-w_write_beside_argument

        .globl  read_w_write
        .type   read_w_write,%function
read_w_write:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      w_write_beside_argument
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_w_write, .-read_w_write

// No argument and no write. A caller that still defines x0 at this call is
// not receiving a value the leaf computed.
        .globl  drop_x0_result
        .type   drop_x0_result,%function
drop_x0_result:
        ret
        .size   drop_x0_result, .-drop_x0_result

// The frame keeps the call from becoming a tail call. The leaf preserves x0,
// so this function returns nothing, and the call edge is not the argument.
        .globl  clobber_x0
        .type   clobber_x0,%function
clobber_x0:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      drop_x0_result
        ldp     x29, x30, [sp], #16
        ret
        .size   clobber_x0, .-clobber_x0

        .globl  void_call_is_not_the_argument
        .type   void_call_is_not_the_argument,%function
void_call_is_not_the_argument:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        cmp     w1, #1
        b.ne    .Lvoid_skip
        bl      clobber_x0
.Lvoid_skip:
        ldp     x29, x30, [sp], #16
        ret
        .size   void_call_is_not_the_argument, .-void_call_is_not_the_argument

        .globl  read_void_call
        .type   read_void_call,%function
read_void_call:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      void_call_is_not_the_argument
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_void_call, .-read_void_call

        .globl  loaded_side_is_not_the_argument
        .type   loaded_side_is_not_the_argument,%function
loaded_side_is_not_the_argument:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        cmp     w1, #1
        b.ne    .Lloaded_x0
        bl      return_nine
        b       .Lloaded_join
.Lloaded_x0:
        ldr     x0, [x2]
.Lloaded_join:
        ldp     x29, x30, [sp], #16
        ret
        .size   loaded_side_is_not_the_argument, .-loaded_side_is_not_the_argument

// No argument register is read. The skip path returns the x0 the function
// was entered with. The call path returns that call. x19 is only a condition.
        .globl  unread_argument_survives_beside_a_call
        .type   unread_argument_survives_beside_a_call,%function
unread_argument_survives_beside_a_call:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        cbz     x19, .Lunread_skip
        bl      return_nine
.Lunread_skip:
        ldp     x29, x30, [sp], #16
        ret
        .size   unread_argument_survives_beside_a_call, .-unread_argument_survives_beside_a_call

        .globl  read_unread_argument
        .type   read_unread_argument,%function
read_unread_argument:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      unread_argument_survives_beside_a_call
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_unread_argument, .-read_unread_argument

// Same shape, but the call leaves no result. That edge is not x0's argument.
        .globl  unread_void_call_is_not_the_argument
        .type   unread_void_call_is_not_the_argument,%function
unread_void_call_is_not_the_argument:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        cbz     x19, .Lunread_void_skip
        bl      clobber_x0
.Lunread_void_skip:
        ldp     x29, x30, [sp], #16
        ret
        .size   unread_void_call_is_not_the_argument, .-unread_void_call_is_not_the_argument

        .globl  read_unread_void_call
        .type   read_unread_void_call,%function
read_unread_void_call:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      unread_void_call_is_not_the_argument
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_unread_void_call, .-read_unread_void_call

// A leaf that stores through x1 and never writes x0 leaves the caller's x0
// in place. The next call still receives that argument.
        .globl  preserve_x0_leaf
        .type   preserve_x0_leaf,%function
preserve_x0_leaf:
        str     xzr, [x1]
        ret
        .size   preserve_x0_leaf, .-preserve_x0_leaf

        .globl  consume_live_x0
        .type   consume_live_x0,%function
consume_live_x0:
        add     x0, x0, #1
        ret
        .size   consume_live_x0, .-consume_live_x0

// The leaf keeps x0, so this function returns the argument it was entered
// with. The call is not that result.
        .globl  return_x0_after_preserving_leaf
        .type   return_x0_after_preserving_leaf,%function
return_x0_after_preserving_leaf:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      preserve_x0_leaf
        ldp     x29, x30, [sp], #16
        ret
        .size   return_x0_after_preserving_leaf, .-return_x0_after_preserving_leaf

        .globl  read_preserved_return
        .type   read_preserved_return,%function
read_preserved_return:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      return_x0_after_preserving_leaf
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size   read_preserved_return, .-read_preserved_return

        .globl  pass_x0_across_preserving_leaf
        .type   pass_x0_across_preserving_leaf,%function
pass_x0_across_preserving_leaf:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      preserve_x0_leaf
        bl      consume_live_x0
        ldp     x29, x30, [sp], #16
        ret
        .size   pass_x0_across_preserving_leaf, .-pass_x0_across_preserving_leaf

// The callee calls a leaf that returns nothing. That call still defines x0
// for this function's callers, so the incoming argument does not reach the
// next call.
        .globl  clobber_x0_by_call
        .type   clobber_x0_by_call,%function
clobber_x0_by_call:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      drop_x0_result
        ldp     x29, x30, [sp], #16
        ret
        .size   clobber_x0_by_call, .-clobber_x0_by_call

        .globl  pass_x0_across_clobbering_call
        .type   pass_x0_across_clobbering_call,%function
pass_x0_across_clobbering_call:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        bl      clobber_x0_by_call
        bl      consume_live_x0
        ldp     x29, x30, [sp], #16
        ret
        .size   pass_x0_across_clobbering_call, .-pass_x0_across_clobbering_call
