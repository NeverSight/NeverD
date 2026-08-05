/* x86-32 stack, call convention, and memory instruction tests */

int test_push_pop(int val) {
    int result;
    __asm__ volatile (
        "pushl %1\n\t"
        "popl %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

void test_pushfd_popfd(void) {
    __asm__ volatile (
        "pushfl\n\t"
        "popfl"
        ::: "memory"
    );
}

int test_xchg32(int a, int b) {
    int r1, r2;
    __asm__ volatile (
        "xchgl %0, %1"
        : "=r"(r1), "=r"(r2)
        : "0"(a), "1"(b)
    );
    return r1;
}

unsigned int test_bswap32(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bswapl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_cmov32(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmovgl %2, %0"
        : "=r"(result)
        : "0"(c), "r"(a), "r"(b)
    );
    return result;
}

int test_setcc32(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setl %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_adc32(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb32(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cbw_cwde(short a) {
    int result;
    __asm__ volatile (
        "movw %1, %%ax\n\t"
        "cwde\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(a)
        : "eax"
    );
    return result;
}

void _start(void) {
    __asm__ volatile (
        "movl $1, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80"
        ::: "eax", "ebx"
    );
}
