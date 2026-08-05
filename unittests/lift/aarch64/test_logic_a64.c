/* AArch64 logic and comparison instruction semantics */

int test_and_a64(int a, int b) {
    int result;
    __asm__ volatile ("and %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_orr_a64(int a, int b) {
    int result;
    __asm__ volatile ("orr %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_eor_a64(int a, int b) {
    int result;
    __asm__ volatile ("eor %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_orn_a64(int a, int b) {
    int result;
    __asm__ volatile ("orn %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_bic_a64(int a, int b) {
    int result;
    __asm__ volatile ("bic %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_eon_a64(int a, int b) {
    int result;
    __asm__ volatile ("eon %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_mvn_a64(int a) {
    int result;
    __asm__ volatile ("mvn %w0, %w1" : "=r"(result) : "r"(a));
    return result;
}

int test_neg_a64(int a) {
    int result;
    __asm__ volatile ("neg %w0, %w1" : "=r"(result) : "r"(a));
    return result;
}

int test_tst_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "tst %w1, %w2\n\t"
        "cset %w0, ne"
        : "=r"(result) : "r"(a), "r"(b));
    return result;
}

long long test_and64_a64(long long a, long long b) {
    long long result;
    __asm__ volatile ("and %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

long long test_orr64_a64(long long a, long long b) {
    long long result;
    __asm__ volatile ("orr %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

void _start(void) {
    __asm__ volatile ("mov x0, #0\n\tmov x8, #93\n\tsvc #0");
}
