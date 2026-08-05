/* ARM32 arithmetic and data processing instruction tests */

int test_add_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "add %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_sub_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "sub %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_rsb_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "rsb %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_and_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "and %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_orr_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "orr %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_eor_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "eor %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_bic_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "bic %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_mvn_arm(int a) {
    int result;
    __asm__ volatile (
        "mvn %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_mov_arm(int a) {
    int result;
    __asm__ volatile (
        "mov %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_lsl_arm(int a, int cnt) {
    int result;
    __asm__ volatile (
        "lsl %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

int test_lsr_arm(unsigned int a, int cnt) {
    unsigned int result;
    __asm__ volatile (
        "lsr %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

int test_asr_arm(int a, int cnt) {
    int result;
    __asm__ volatile (
        "asr %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

int test_mul_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "mul %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_mla_arm(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "mla %0, %1, %2, %3"
        : "=r"(result)
        : "r"(a), "r"(b), "r"(c)
    );
    return result;
}

int test_add_imm_arm(int a) {
    int result;
    __asm__ volatile (
        "add %0, %1, #42"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_lsl_imm_arm(int a) {
    int result;
    __asm__ volatile (
        "lsl %0, %1, #3"
        : "=r"(result)
        : "r"(a)
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
