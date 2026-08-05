/* ARM32 multiply and accumulate instruction semantics */

int test_mul_arm(int a, int b) {
    int result;
    __asm__ volatile ("mul %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int test_mla_arm(int a, int b, int c) {
    int result;
    __asm__ volatile ("mla %0, %1, %2, %3" : "=r"(result) : "r"(a), "r"(b), "r"(c));
    return result;
}

int test_mls_arm(int a, int b, int c) {
    int result;
    __asm__ volatile ("mls %0, %1, %2, %3" : "=r"(result) : "r"(a), "r"(b), "r"(c));
    return result;
}

int test_neg_arm(int a) {
    int result;
    __asm__ volatile ("rsb %0, %1, #0" : "=r"(result) : "r"(a));
    return result;
}

void _start(void) {
    __asm__ volatile (
        "mov r0, #0\n\t"
        "mov r7, #1\n\t"
        "svc #0"
        ::: "r0", "r7"
    );
}
