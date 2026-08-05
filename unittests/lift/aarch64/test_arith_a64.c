/* AArch64 arithmetic instruction semantics */

int test_add_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "add %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_sub_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "sub %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_mul_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "mul %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_and_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "and %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_orr_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "orr %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_eor_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "eor %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_neg_a64(int a) {
    int result;
    __asm__ volatile (
        "neg %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_mvn_a64(int a) {
    int result;
    __asm__ volatile (
        "mvn %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

long long test_add64_a64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "add %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_lsl_a64(int a, int count) {
    int result;
    __asm__ volatile (
        "lsl %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(count)
    );
    return result;
}

int test_lsr_a64(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "lsr %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(count)
    );
    return result;
}

int test_asr_a64(int a, int count) {
    int result;
    __asm__ volatile (
        "asr %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(count)
    );
    return result;
}

int test_sdiv_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "sdiv %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

unsigned int test_udiv_a64(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "udiv %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_madd_a64(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "madd %w0, %w1, %w2, %w3"
        : "=r"(result)
        : "r"(a), "r"(b), "r"(c)
    );
    return result;
}

void _start(void) {
    __asm__ volatile (
        "mov x8, #93\n\t"
        "mov x0, #0\n\t"
        "svc #0"
    );
}
