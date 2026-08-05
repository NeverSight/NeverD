/* ARM32 extend and byte ops: SXTB, SXTH, UXTB, UXTH, REV, CLZ, BFI, BFC */

int test_sxtb_arm(int val) {
    int result;
    __asm__ volatile (
        "sxtb %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_sxth_arm(int val) {
    int result;
    __asm__ volatile (
        "sxth %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_uxtb_arm(int val) {
    int result;
    __asm__ volatile (
        "uxtb %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_uxth_arm(int val) {
    int result;
    __asm__ volatile (
        "uxth %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_rev_arm(int val) {
    int result;
    __asm__ volatile (
        "rev %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_rev16_arm(int val) {
    int result;
    __asm__ volatile (
        "rev16 %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_clz_arm(int val) {
    int result;
    __asm__ volatile (
        "clz %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_rbit_arm(int val) {
    int result;
    __asm__ volatile (
        "rbit %0, %1"
        : "=r"(result)
        : "r"(val)
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
