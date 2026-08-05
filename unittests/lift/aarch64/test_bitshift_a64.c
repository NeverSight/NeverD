/* AArch64: bit operations, shifts, extracts, reverses */

int test_clz_a64(unsigned int a) {
    int result;
    __asm__ volatile (
        "clz %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_cls_a64(int a) {
    int result;
    __asm__ volatile (
        "cls %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_rbit_a64(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rbit %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_rev_a64(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rev %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_rev16_a64(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rev16 %w0, %w1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned long long test_rev64_a64(unsigned long long a) {
    unsigned long long result;
    __asm__ volatile (
        "rev %0, %1"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_ror_a64(unsigned int a, int cnt) {
    unsigned int result;
    __asm__ volatile (
        "ror %w0, %w1, %w2"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

int test_extr_a64(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "extr %w0, %w1, %w2, #8"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_bfm_a64(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "ubfx %w0, %w1, #4, #8"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_sbfx_a64(int a) {
    int result;
    __asm__ volatile (
        "sbfx %w0, %w1, #4, #8"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_bfi_a64(unsigned int a, unsigned int b) {
    unsigned int result = a;
    __asm__ volatile (
        "bfi %w0, %w1, #4, #8"
        : "+r"(result)
        : "r"(b)
    );
    return result;
}

int test_tst_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "tst %w1, %w2\n\t"
        "cset %w0, eq"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmn_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "cmn %w1, %w2\n\t"
        "cset %w0, eq"
        : "=r"(result)
        : "r"(a), "r"(b)
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
