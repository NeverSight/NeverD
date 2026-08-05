/* x86-64 carry/borrow operations: ADC, SBB */

int test_adc(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_adc_with_carry(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb_with_borrow(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

long long test_adc64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "clc\n\t"
        "adcq %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

long long test_sbb64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "clc\n\t"
        "sbbq %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
