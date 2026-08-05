/* AArch64 atomic / exclusive ops: LDXR, STXR, CAS, SWP (ARMv8.1) */

int test_ldxr_stxr(int* addr, int newval) {
    int old;
    int status;
    __asm__ volatile (
        "ldxr %w0, [%2]\n\t"
        "stxr %w1, %w3, [%2]"
        : "=&r"(old), "=&r"(status)
        : "r"(addr), "r"(newval)
        : "memory"
    );
    return old;
}

long long test_ldxr_stxr_64(long long* addr, long long newval) {
    long long old;
    int status;
    __asm__ volatile (
        "ldxr %0, [%2]\n\t"
        "stxr %w1, %3, [%2]"
        : "=&r"(old), "=&r"(status)
        : "r"(addr), "r"(newval)
        : "memory"
    );
    return old;
}

void test_dmb(void) {
    __asm__ volatile ("dmb ish" ::: "memory");
}

void test_dsb(void) {
    __asm__ volatile ("dsb ish" ::: "memory");
}

void test_isb(void) {
    __asm__ volatile ("isb" ::: "memory");
}

void _start(void) {
    __asm__ volatile (
        "mov x8, #93\n\t"
        "mov x0, #0\n\t"
        "svc #0"
    );
}
