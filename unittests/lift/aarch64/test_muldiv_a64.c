/* AArch64 multiply and divide instruction semantics */

int test_mul_a64(int a, int b) {
    int result;
    __asm__ volatile ("mul %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

unsigned int test_udiv_a64(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile ("udiv %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_sdiv_a64(int a, int b) {
    int result;
    __asm__ volatile ("sdiv %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_madd_a64(int a, int b, int c) {
    int result;
    __asm__ volatile ("madd %w0, %w1, %w2, %w3" : "=r"(result) : "r"(a), "r"(b), "r"(c));
    return result;
}

int test_msub_a64(int a, int b, int c) {
    int result;
    __asm__ volatile ("msub %w0, %w1, %w2, %w3" : "=r"(result) : "r"(a), "r"(b), "r"(c));
    return result;
}

long long test_smull_a64(int a, int b) {
    long long result;
    __asm__ volatile ("smull %0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

unsigned long long test_umull_a64(unsigned int a, unsigned int b) {
    unsigned long long result;
    __asm__ volatile ("umull %0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

long long test_mul64_a64(long long a, long long b) {
    long long result;
    __asm__ volatile ("mul %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

void _start(void) {
    __asm__ volatile ("mov x0, #0\n\tmov x8, #93\n\tsvc #0");
}
