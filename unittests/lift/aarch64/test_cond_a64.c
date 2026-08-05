/* AArch64 conditional operations: CSEL, CSET, CSINC, CSINV, CSNEG */

int test_csel_a64(int a, int b, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w3, #0\n\t"
        "csel %w0, %w1, %w2, ne"
        : "=r"(result)
        : "r"(a), "r"(b), "r"(cond)
    );
    return result;
}

int test_cset_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "cmp %w1, %w2\n\t"
        "cset %w0, eq"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_csinc_a64(int a, int b, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w3, #0\n\t"
        "csinc %w0, %w1, %w2, eq"
        : "=r"(result)
        : "r"(a), "r"(b), "r"(cond)
    );
    return result;
}

int test_ccmp_a64(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "cmp %w1, %w2\n\t"
        "ccmp %w1, %w3, #0, eq\n\t"
        "cset %w0, lt"
        : "=r"(result)
        : "r"(a), "r"(b), "r"(c)
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
