/* AArch64 conditional select and set instruction semantics */

int test_csel_a64(int a, int b, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w3, #0\n\t"
        "csel %w0, %w1, %w2, ne"
        : "=r"(result) : "r"(a), "r"(b), "r"(cond));
    return result;
}

int test_csinc_a64(int a, int b, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w3, #0\n\t"
        "csinc %w0, %w1, %w2, ne"
        : "=r"(result) : "r"(a), "r"(b), "r"(cond));
    return result;
}

int test_csinv_a64(int a, int b, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w3, #0\n\t"
        "csinv %w0, %w1, %w2, ne"
        : "=r"(result) : "r"(a), "r"(b), "r"(cond));
    return result;
}

int test_csneg_a64(int a, int b, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w3, #0\n\t"
        "csneg %w0, %w1, %w2, ne"
        : "=r"(result) : "r"(a), "r"(b), "r"(cond));
    return result;
}

int test_cset_eq_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "cmp %w1, %w2\n\t"
        "cset %w0, eq"
        : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_csetm_a64(int a, int b) {
    int result;
    __asm__ volatile (
        "cmp %w1, %w2\n\t"
        "csetm %w0, eq"
        : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_cinc_a64(int a, int cond) {
    int result;
    __asm__ volatile (
        "cmp %w2, #0\n\t"
        "cinc %w0, %w1, ne"
        : "=r"(result) : "r"(a), "r"(cond));
    return result;
}

void _start(void) {
    __asm__ volatile ("mov x0, #0\n\tmov x8, #93\n\tsvc #0");
}
