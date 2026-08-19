        .text
        .global _start
        .global body
        .type _start, %function
        .type body, %function
_start:
        sub sp, sp, #128
        stp x29, x30, [sp, #112]
        add x19, sp, #8
        add x20, sp, #40
        mov x0, x19
        mov x1, x20
        mov w2, #7
        adrp x3, text
        add x3, x3, :lo12:text
        str xzr, [x0, #16]
        mov x8, #1
        str x8, [x0, #24]
        str xzr, [x1, #56]
        strh wzr, [x1, #30]
        bl body
        mov x8, #93
        svc #0
        .size _start, .-_start

body:
        ldr w8, [x1, #0x38]
        mov w9, #0x1e
        mov w10, #0x20
        ldp x11, x12, [x0, #0x10]
        cmp x11, x8
        csel x8, x10, x9, lo
        ldrh w8, [x1, x8]
        mov w9, #1
        lsl w8, w9, w8
        cmp x12, w8, sxtw
        b.ne .Lassert
        sxtw x0, w8
        ret

.Lassert:
        stp x29, x30, [sp, #-0x10]!
        mov x29, sp
        bl assert_helper
        sub sp, sp, #0x20
        stp x29, x30, [sp, #0x10]
        add x29, sp, #0x10
        mov w9, #0x14
        adrp x8, table_entry0
        add x8, x8, :lo12:table_entry0

.Lscan:
        ldur w10, [x8, #-8]
        cmp w10, w2
        b.eq .Lfound
        add x8, x8, #0x10
        subs x9, x9, #1
        b.ne .Lscan
        str x2, [sp]
        adrp x2, fmt_hex
        add x2, x2, :lo12:fmt_hex
        b .Lcall

.Lfound:
        ldr x8, [x8]
        str x8, [sp]
        adrp x2, fmt_string
        add x2, x2, :lo12:fmt_string

.Lcall:
        mov x1, #8
        bl snprintf
        ldp x29, x30, [sp, #0x10]
        add sp, sp, #0x20
        ret
        .size body, .-body

        .type assert_helper, %function
assert_helper:
        ret
        .size assert_helper, .-assert_helper

        .section .rodata,"a",%progbits
        .p2align 3
table:
        .long 7
        .long 0
table_entry0:
        .quad 0
fmt_hex:
        .asciz "%#x"
fmt_string:
        .asciz "%s"
text:
        .asciz "1234567"
