/* x86-32 arithmetic instruction semantics (i386 target) */

int test_add32(int a, int b) {
    int result;
    __asm__ volatile (
        "addl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sub32(int a, int b) {
    int result;
    __asm__ volatile (
        "subl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_imul32(int a, int b) {
    int result;
    __asm__ volatile (
        "imull %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
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

int test_shl32(int a, int cnt) {
    int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "shll %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(cnt)
        : "ecx"
    );
    return result;
}

int test_shr32(unsigned int a, int cnt) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "shrl %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(cnt)
        : "ecx"
    );
    return result;
}

int test_and32(int a, int b) {
    int result;
    __asm__ volatile (
        "andl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_or32(int a, int b) {
    int result;
    __asm__ volatile (
        "orl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_xor32(int a, int b) {
    int result;
    __asm__ volatile (
        "xorl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_neg32(int a) {
    int result;
    __asm__ volatile (
        "negl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_not32(int a) {
    int result;
    __asm__ volatile (
        "notl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_push_pop32(int val) {
    int result;
    __asm__ volatile (
        "pushl %1\n\t"
        "popl %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_lea32(int a, int b) {
    int result;
    __asm__ volatile (
        "leal (%1, %2, 4), %0"
        : "=r"(result)
        : "r"(a), "r"(b)
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
