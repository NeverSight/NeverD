// Return the full GPR in r3. The caller also receives CR0 and CR1 in memory.
// CR1 compares the defined low word and reads SO after the multiply.
        .text
#if defined(__powerpc64__)
        .abiversion 2
#define STORE std
#else
#define STORE stw
#endif

        .macro product name, instruction
        .globl \name
        .type \name,@function
\name:
        mtcrf 128,3
        mtxer 4
        \instruction 3,3,4
        cmpwi 1,3,0
        mfcr 6
        srwi 6,6,24
        STORE 6,0(5)
        blr
        .size \name,.-\name
        .endm

        product high_signed, mulhw
        product high_unsigned, mulhwu
        product high_signed_record, mulhw.
        product high_unsigned_record, mulhwu.
        product word_overflow, mullwo
        product word_overflow_record, mullwo.
#if defined(__powerpc64__)
        product double_overflow, mulldo
        product double_overflow_record, mulldo.

        .globl repeated_high
        .type repeated_high,@function
repeated_high:
        mulhw. 6,3,4
        mfcr 7
        mulhw. 3,3,4
        mfcr 8
        xor 3,6,3
        xor 7,7,8
        srwi 7,7,29
        STORE 7,0(5)
        blr
        .size repeated_high,.-repeated_high
#endif
