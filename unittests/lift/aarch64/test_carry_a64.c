/* AArch64 carry/borrow: ADC, SBC, NGC, ADDS/SUBS flags */

long long test_adc_a64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "adds xzr, xzr, xzr\n\t"
        "adc %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

long long test_sbc_a64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "subs xzr, xzr, xzr\n\t"
        "sbc %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

long long test_adcs_a64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "adds xzr, xzr, xzr\n\t"
        "adcs %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

int test_adds_w(int a, int b) {
    int result;
    __asm__ volatile (
        "adds %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

int test_subs_w(int a, int b) {
    int result;
    __asm__ volatile (
        "subs %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

long long test_neg_a64(long long a) {
    long long result;
    __asm__ volatile (
        "neg %0, %1"
        : "=r"(result)
        : "r"(a)
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
