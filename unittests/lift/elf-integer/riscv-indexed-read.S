        .text
        .option norvc
        .globl indexed_read
        .type indexed_read, @function
indexed_read:
        lui   t0, %hi(table)
        addi  t0, t0, %lo(table)
        andi  t1, a0, 31
        add   t0, t0, t1
        lbu   a0, 0(t0)
        xor   a0, a0, t0
        ret
        .size indexed_read, .-indexed_read

        .section .rodata,"a",@progbits
        .type table, @object
table:
        .set n, 32
        .rept 32
        .byte n
        .set n, n-1
        .endr
        .size table, .-table
