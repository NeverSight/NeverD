/* AArch64 extension and conversion: SXTB, SXTH, SXTW, UXTB, UXTH, REV, CLZ */

long long test_sxtb_a64(int val) {
    long long result;
    __asm__ volatile (
        "sxtb %0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

long long test_sxth_a64(int val) {
    long long result;
    __asm__ volatile (
        "sxth %0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

long long test_sxtw_a64(int val) {
    long long result;
    __asm__ volatile (
        "sxtw %0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_uxtb_a64(int val) {
    int result;
    __asm__ volatile (
        "uxtb %w0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_uxth_a64(int val) {
    int result;
    __asm__ volatile (
        "uxth %w0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

long long test_rev_a64(long long val) {
    long long result;
    __asm__ volatile (
        "rev %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_rev_w(int val) {
    int result;
    __asm__ volatile (
        "rev %w0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_clz_w(int val) {
    int result;
    __asm__ volatile (
        "clz %w0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

long long test_clz_a64(long long val) {
    long long result;
    __asm__ volatile (
        "clz %0, %1"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_rbit_w(int val) {
    int result;
    __asm__ volatile (
        "rbit %w0, %w1"
        : "=r"(result)
        : "r"(val)
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
