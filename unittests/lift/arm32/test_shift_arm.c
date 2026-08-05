/* ARM32 shift and rotate: LSL, LSR, ASR, ROR, flexible second operand */

int test_lsl_imm(int val) {
    int result;
    __asm__ volatile (
        "lsl %0, %1, #4"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_lsr_imm(unsigned int val) {
    unsigned int result;
    __asm__ volatile (
        "lsr %0, %1, #4"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_asr_imm(int val) {
    int result;
    __asm__ volatile (
        "asr %0, %1, #4"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_ror_imm(unsigned int val) {
    unsigned int result;
    __asm__ volatile (
        "ror %0, %1, #8"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_lsl_reg(int val, int amt) {
    int result;
    __asm__ volatile (
        "lsl %0, %1, %2"
        : "=r"(result)
        : "r"(val), "r"(amt)
    );
    return result;
}

int test_add_shifted(int a, int b) {
    int result;
    __asm__ volatile (
        "add %0, %1, %2, lsl #2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_sub_shifted(int a, int b) {
    int result;
    __asm__ volatile (
        "sub %0, %1, %2, asr #3"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_mvn(int val) {
    int result;
    __asm__ volatile (
        "mvn %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

void _start(void) {
    __asm__ volatile (
        "mov r7, #1\n\t"
        "mov r0, #0\n\t"
        "svc #0"
        ::: "r0", "r7"
    );
}
