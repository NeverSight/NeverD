/* ARM32 carry/borrow: ADC, SBC, RSC, ADDS/SUBS flags */

int test_adc_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "adds r0, r0, #0\n\t"
        "adc %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc", "r0"
    );
    return result;
}

int test_sbc_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "subs r0, r0, r0\n\t"
        "sbc %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc", "r0"
    );
    return result;
}

int test_rsc_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "subs r0, r0, r0\n\t"
        "rsc %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc", "r0"
    );
    return result;
}

int test_adds_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "adds %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

int test_subs_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "subs %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "cc"
    );
    return result;
}

int test_neg_arm(int a) {
    int result;
    __asm__ volatile (
        "rsb %0, %1, #0"
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
