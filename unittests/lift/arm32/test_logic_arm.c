/* ARM32 logic and bit manipulation instruction semantics */

int test_and_arm(int a, int b) {
    int result;
    __asm__ volatile ("and %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_orr_arm(int a, int b) {
    int result;
    __asm__ volatile ("orr %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_eor_arm(int a, int b) {
    int result;
    __asm__ volatile ("eor %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_bic_arm(int a, int b) {
    int result;
    __asm__ volatile ("bic %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_mvn_arm(int a) {
    int result;
    __asm__ volatile ("mvn %0, %1" : "=r"(result) : "r"(a));
    return result;
}

int test_lsl_arm(int a, int b) {
    int result;
    __asm__ volatile ("lsl %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_lsr_arm(unsigned int a, int b) {
    unsigned int result;
    __asm__ volatile ("lsr %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_asr_arm(int a, int b) {
    int result;
    __asm__ volatile ("asr %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_ror_arm(unsigned int a, int b) {
    unsigned int result;
    __asm__ volatile ("ror %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_rev_arm(int a) {
    int result;
    __asm__ volatile ("rev %0, %1" : "=r"(result) : "r"(a));
    return result;
}

int test_clz_arm(int a) {
    int result;
    __asm__ volatile ("clz %0, %1" : "=r"(result) : "r"(a));
    return result;
}

void _start(void) {
    __asm__ volatile (
        "mov r0, #0\n\t"
        "mov r7, #1\n\t"
        "svc #0"
        ::: "r0", "r7"
    );
}
