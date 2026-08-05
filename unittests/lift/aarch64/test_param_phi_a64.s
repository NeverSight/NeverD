// The fallthrough path creates a new W0 SSA value while the taken path keeps
// the incoming W0.  The merge therefore consumes the parameter through a PHI.
    .text
    .globl test_param_phi
    .type test_param_phi, %function
test_param_phi:
    cbz w1, .Lmerge
    mov w0, w0
.Lmerge:
    add w0, w0, #1
    ret
    .size test_param_phi, .-test_param_phi

// A narrow load result must not narrow the incoming address parameter.
    .globl test_param_load32
    .type test_param_load32, %function
test_param_load32:
    ldr w0, [x0]
    ret
    .size test_param_load32, .-test_param_load32

    .type test_call_leaf, %function
test_call_leaf:
    add x0, x0, #1
    ret
    .size test_call_leaf, .-test_call_leaf

// x1 is caller-saved.  Branching on it only after a call must not make it an
// incoming parameter of this function, and the unknown value stays observable.
    .globl test_post_call_x1
    .type test_post_call_x1, %function
test_post_call_x1:
    str x30, [sp, #-16]!
    mov x0, #0
    bl test_call_leaf
    cbz x1, .Lzero
    cbnz x1, .Lone
    mov x0, #2
    b .Ldone
.Lone:
    mov x0, #1
    b .Ldone
.Lzero:
    mov x0, #0
.Ldone:
    ldr x30, [sp], #16
    ret
    .size test_post_call_x1, .-test_post_call_x1

// The explicit x0 call result also defines its w0 alias.  A post-call narrow
// read must not reconnect to the pre-call w0 SSA value.
    .globl test_post_call_w0
    .type test_post_call_w0, %function
test_post_call_w0:
    str x30, [sp, #-16]!
    mov x0, #41
    bl test_call_leaf
    add w0, w0, #1
    ldr x30, [sp], #16
    ret
    .size test_post_call_w0, .-test_post_call_w0

// Flag micro-ops emitted for each ADD share that ADD's address and do not
// consume its result.  A later real CMP has a distinct address and does consume
// x1, so this remains a scalar return rather than a false x0:x1 pair.
    .globl test_late_cmp_consumes_x1
    .type test_late_cmp_consumes_x1, %function
test_late_cmp_consumes_x1:
    add x0, x0, #1
    add x1, x1, #2
    cmp x1, #0
    ret
    .size test_late_cmp_consumes_x1, .-test_late_cmp_consumes_x1

// AAPCS64 preserves the low 64 bits of v8-v15.
    .globl test_post_call_d9
    .type test_post_call_d9, %function
test_post_call_d9:
    str x30, [sp, #-16]!
    mov x2, #123
    fmov d9, x2
    mov x0, #0
    bl test_call_leaf
    fmov x0, d9
    ldr x30, [sp], #16
    ret
    .size test_post_call_d9, .-test_post_call_d9

// The upper 64 bits of the same physical v9 register remain volatile.
    .globl test_post_call_q9_upper
    .type test_post_call_q9_upper, %function
test_post_call_q9_upper:
    str x30, [sp, #-16]!
    mov x0, #0
    bl test_call_leaf
    umov x0, v9.d[1]
    ldr x30, [sp], #16
    ret
    .size test_post_call_q9_upper, .-test_post_call_q9_upper
