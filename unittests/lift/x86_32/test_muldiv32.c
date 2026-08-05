/* x86-32 multiply/divide instruction tests */

int test_imul32_2op(int a, int b) {
    int result;
    __asm__ volatile (
        "imull %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_imul32_3op(int a, int b) {
    int result;
    __asm__ volatile (
        "imull $7, %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    (void)b;
    return result;
}

unsigned int test_mul32(unsigned int a, unsigned int b) {
    unsigned int lo;
    __asm__ volatile (
        "mull %2"
        : "=a"(lo)
        : "0"(a), "r"(b)
        : "edx"
    );
    return lo;
}

unsigned int test_div32(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "xorl %%edx, %%edx\n\t"
        "divl %2"
        : "=a"(result)
        : "0"(a), "r"(b)
        : "edx"
    );
    return result;
}

int test_idiv32(int a, int b) {
    int result;
    __asm__ volatile (
        "cdq\n\t"
        "idivl %2"
        : "=a"(result)
        : "0"(a), "r"(b)
        : "edx"
    );
    return result;
}

unsigned int test_mod32(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "xorl %%edx, %%edx\n\t"
        "divl %2\n\t"
        "movl %%edx, %0"
        : "=r"(result)
        : "a"(a), "r"(b)
        : "edx"
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
