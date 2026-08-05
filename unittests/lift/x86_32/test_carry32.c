/* x86-32 carry/borrow operations: ADC, SBB */

int test_adc32(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_adc32_with_carry(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb32(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb32_with_borrow(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_adc_chain(int lo_a, int hi_a, int lo_b, int hi_b) {
    int lo_r, hi_r;
    __asm__ volatile (
        "addl %4, %0\n\t"
        "adcl %5, %1"
        : "=r"(lo_r), "=r"(hi_r)
        : "0"(lo_a), "1"(hi_a), "r"(lo_b), "r"(hi_b)
    );
    return hi_r;
}

void _start(void) {
    __asm__ volatile (
        "movl $1, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80"
        ::: "eax", "ebx"
    );
}
