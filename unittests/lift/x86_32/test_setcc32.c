/* x86-32 conditional set: SETcc, CMOVcc */

int test_sete32(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "sete %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "eax"
    );
    return result;
}

int test_setl32(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setl %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "eax"
    );
    return result;
}

int test_setg32(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setg %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "eax"
    );
    return result;
}

int test_setb32(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setb %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "eax"
    );
    return result;
}

int test_seta32(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "seta %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "eax"
    );
    return result;
}

int test_sets32(int a) {
    int result;
    __asm__ volatile (
        "testl %1, %1\n\t"
        "sets %%al\n\t"
        "movzbl %%al, %0"
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
