/* ARM32: control flow, conditional execution, bit operations */

int test_cmp_beq_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "cmp %1, %2\n\t"
        "moveq %0, #1\n\t"
        "movne %0, #0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmp_bgt_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "cmp %1, %2\n\t"
        "movgt %0, #1\n\t"
        "movle %0, #0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_tst_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "tst %1, %2\n\t"
        "moveq %0, #1\n\t"
        "movne %0, #0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmn_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "cmn %1, %2\n\t"
        "moveq %0, #1\n\t"
        "movne %0, #0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

unsigned int test_clz_arm(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "clz %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_rev_arm(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rev %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_rev16_arm(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rev16 %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_sxtb_arm(int a) {
    int result;
    __asm__ volatile (
        "sxtb %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_sxth_arm(int a) {
    int result;
    __asm__ volatile (
        "sxth %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_uxtb_arm(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "uxtb %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_uxth_arm(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "uxth %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_adc_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "adds %0, %1, %2\n\t"
        "adc %0, %0, #0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_sbc_arm(int a, int b) {
    int result;
    __asm__ volatile (
        "subs %0, %1, %2\n\t"
        "sbc %0, %0, #0"
        : "=r"(result)
        : "r"(a), "r"(b)
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
