// The optimized AArch64 computed-goto shape: one entry ADRP/ADD defines the
// shared table base and five register-offset loads (entry plus four target
// bodies) each feed the indirect branch of their own block.  The loads are
// retained as original memory operations, so the constant-base strategy is the
// one that recovers them and inventories the tail-duplicated siblings.  Each
// branch must publish only its own load and the exact index input of that
// load: a sibling's occurrence is a member of the consumer group, never a
// selector of this branch.
//
// The entry body overwrites its index register between the load and the
// branch, and every target body loads the entry back into its index register,
// so a selector taken from the register at the branch would be wrong.
        .text
        .p2align 2
        .globl  a64_sibling_checked_loads
        .type   a64_sibling_checked_loads, %function
a64_sibling_checked_loads:
        .globl  a64_sibling_checked_entry_begin
a64_sibling_checked_entry_begin:
        adrp    x11, a64_sibling_checked_table
        add     x11, x11, :lo12:a64_sibling_checked_table
        mov     w10, wzr
        and     w9, w0, #0x3
        ldr     x12, [x11, w9, uxtw #3]
        mov     w9, #1
        br      x12
        .globl  a64_sibling_checked_entry_end
a64_sibling_checked_entry_end:

        .globl  a64_sibling_checked_t0_begin
a64_sibling_checked_t0_begin:
        add     w0, w0, w0, lsl #2
        cmp     w9, #63
        b.hi    a64_sibling_checked_done
        add     w9, w9, #1
        lsr     w12, w0, #2
        and     w12, w12, #0x3
        ldr     x12, [x11, w12, uxtw #3]
        br      x12
        .globl  a64_sibling_checked_t0_end
a64_sibling_checked_t0_end:

        .globl  a64_sibling_checked_t1_begin
a64_sibling_checked_t1_begin:
        eor     w0, w0, w0, lsr #17
        cmp     w9, #63
        b.hi    a64_sibling_checked_done
        add     w9, w9, #1
        lsr     w12, w0, #5
        and     w12, w12, #0x3
        ldr     x12, [x11, w12, uxtw #3]
        br      x12
        .globl  a64_sibling_checked_t1_end
a64_sibling_checked_t1_end:

        .globl  a64_sibling_checked_t2_begin
a64_sibling_checked_t2_begin:
        ror     w0, w0, #19
        cmp     w9, #63
        b.hi    a64_sibling_checked_done
        add     w9, w9, #1
        lsr     w12, w0, #9
        and     w12, w12, #0x3
        ldr     x12, [x11, w12, uxtw #3]
        br      x12
        .globl  a64_sibling_checked_t2_end
a64_sibling_checked_t2_end:

        .globl  a64_sibling_checked_t3_begin
a64_sibling_checked_t3_begin:
        sub     w0, w0, w0, lsl #7
        cmp     w9, #63
        b.hi    a64_sibling_checked_done
        add     w9, w9, #1
        lsr     w12, w0, #1
        and     w12, w12, #0x3
        ldr     x12, [x11, w12, uxtw #3]
        br      x12
        .globl  a64_sibling_checked_t3_end
a64_sibling_checked_t3_end:

a64_sibling_checked_done:
        eor     w0, w0, w9
        ret
        .size   a64_sibling_checked_loads, .-a64_sibling_checked_loads

        .section .data.rel.ro,"aw",%progbits
        .p2align 3
        .globl  a64_sibling_checked_table
        .type   a64_sibling_checked_table, %object
a64_sibling_checked_table:
        .xword  a64_sibling_checked_t0_begin
        .xword  a64_sibling_checked_t1_begin
        .xword  a64_sibling_checked_t2_begin
        .xword  a64_sibling_checked_t3_begin
        .size   a64_sibling_checked_table, 32

// A dispatch with its own same-block table load, and an unrelated read of the
// same table in one direct predecessor block.  The predecessor's read feeds no
// branch: it is not a role of the dispatch either.
        .text
        .p2align 2
        .globl  a64_checked_load_beside_dispatch
        .type   a64_checked_load_beside_dispatch, %function
a64_checked_load_beside_dispatch:
        adrp    x11, a64_beside_dispatch_table
        add     x11, x11, :lo12:a64_beside_dispatch_table
        and     w9, w0, #0x3
        tbz     w0, #4, .La64_beside_dispatch
        lsr     w10, w0, #5
        and     w10, w10, #0x3
        ldr     x13, [x11, w10, uxtw #3]
        add     w0, w0, #7
        b       .La64_beside_dispatch
        .globl  a64_beside_dispatch_begin
a64_beside_dispatch_begin:
.La64_beside_dispatch:
        ldr     x12, [x11, w9, uxtw #3]
        br      x12
        .globl  a64_beside_dispatch_end
a64_beside_dispatch_end:
.La64_beside_t0:
        add     w0, w0, #100
        ret
.La64_beside_t1:
        add     w0, w0, #200
        ret
.La64_beside_t2:
        add     w0, w0, #300
        ret
.La64_beside_t3:
        add     w0, w0, #400
        ret
        .size   a64_checked_load_beside_dispatch, .-a64_checked_load_beside_dispatch

// A separate section: this file does not test two label tables that share an
// owner.
        .section .data.rel.ro.beside,"aw",%progbits
        .p2align 3
        .type   a64_beside_dispatch_table, %object
a64_beside_dispatch_table:
        .xword  .La64_beside_t0
        .xword  .La64_beside_t1
        .xword  .La64_beside_t2
        .xword  .La64_beside_t3
        .size   a64_beside_dispatch_table, 32
