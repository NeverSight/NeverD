/* AArch64 memory operations: LDR, STR, LDP, STP, extensions */

int test_ldr_str_a64(int val) {
    int result;
    int buf;
    __asm__ volatile (
        "str %w1, [%2]\n\t"
        "ldr %w0, [%2]"
        : "=r"(result)
        : "r"(val), "r"(&buf)
        : "memory"
    );
    return result;
}

long long test_ldrsw_a64(int val) {
    long long result;
    __asm__ volatile (
        "str %w1, [%2]\n\t"
        "ldrsw %0, [%2]"
        : "=r"(result)
        : "r"(val), "r"(&(int){0})
        : "memory"
    );
    return result;
}

int test_ldrb_a64(int val) {
    int result;
    unsigned char buf;
    __asm__ volatile (
        "strb %w1, [%2]\n\t"
        "ldrb %w0, [%2]"
        : "=r"(result)
        : "r"(val), "r"(&buf)
        : "memory"
    );
    return result;
}

int test_ldrh_a64(int val) {
    int result;
    unsigned short buf;
    __asm__ volatile (
        "strh %w1, [%2]\n\t"
        "ldrh %w0, [%2]"
        : "=r"(result)
        : "r"(val), "r"(&buf)
        : "memory"
    );
    return result;
}

int test_sxtw_a64(int val) {
    long long result;
    __asm__ volatile (
        "sxtw %0, %w1"
        : "=r"(result)
        : "r"(val)
    );
    return (int)result;
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

void _start(void) {
    __asm__ volatile (
        "mov x8, #93\n\t"
        "mov x0, #0\n\t"
        "svc #0"
    );
}
