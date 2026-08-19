        .section __TEXT,__text,regular,pure_instructions
        .globl _main
        .globl _body
        .p2align 2
_main:
        sub sp, sp, #128
        stp x29, x30, [sp, #112]
        add x19, sp, #8
        add x20, sp, #40
        mov x0, x19
        mov x1, x20
        mov w2, #7
        adrp x3, _text@PAGE
        add x3, x3, _text@PAGEOFF
        str xzr, [x0, #16]
        mov x8, #1
        str x8, [x0, #24]
        str xzr, [x1, #56]
        strh wzr, [x1, #30]
        bl _body
        ldp x29, x30, [sp, #112]
        add sp, sp, #128
        ret

        .p2align 2
_body:
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
        b.ne Lassert
        sxtw x0, w8
        ret

Lassert:
        stp x29, x30, [sp, #-0x10]!
        mov x29, sp
        bl _assert_helper
        sub sp, sp, #0x20
        stp x29, x30, [sp, #0x10]
        add x29, sp, #0x10
        mov w9, #0x14
        adrp x8, _table_entry0@PAGE
        add x8, x8, _table_entry0@PAGEOFF

Lscan:
        ldur w10, [x8, #-8]
        cmp w10, w2
        b.eq Lfound
        add x8, x8, #0x10
        subs x9, x9, #1
        b.ne Lscan
        str x2, [sp]
        adrp x2, _fmt_hex@PAGE
        add x2, x2, _fmt_hex@PAGEOFF
        b Lcall

Lfound:
        ldr x8, [x8]
        str x8, [sp]
        adrp x2, _fmt_string@PAGE
        add x2, x2, _fmt_string@PAGEOFF

Lcall:
        mov x1, #8
        bl _snprintf
        ldp x29, x30, [sp, #0x10]
        add sp, sp, #0x20
        ret

        .p2align 2
_assert_helper:
        ret

        .section __DATA_CONST,__const
        .p2align 3
_table:
        .long 7
        .long 0
_table_entry0:
        .quad _text

        .section __TEXT,__cstring
_fmt_hex:
        .asciz "%#x"
_fmt_string:
        .asciz "%s"
_text:
        .asciz "1234567"
