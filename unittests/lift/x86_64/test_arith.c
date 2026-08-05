/* x86-64 arithmetic instruction semantics — lifted IR must preserve these. */

int test_add(int a, int b) {
    int result;
    __asm__ volatile (
        "addl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sub(int a, int b) {
    int result;
    __asm__ volatile (
        "subl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_and(int a, int b) {
    int result;
    __asm__ volatile (
        "andl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_or(int a, int b) {
    int result;
    __asm__ volatile (
        "orl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_xor(int a, int b) {
    int result;
    __asm__ volatile (
        "xorl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_xor_self(int a) {
    int result;
    __asm__ volatile (
        "xorl %0, %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_neg(int a) {
    int result;
    __asm__ volatile (
        "negl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_not(int a) {
    int result;
    __asm__ volatile (
        "notl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_inc(int a) {
    int result;
    __asm__ volatile (
        "incl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_dec(int a) {
    int result;
    __asm__ volatile (
        "decl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_shl(int a, int count) {
    int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "shll %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

int test_shr(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "shrl %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

int test_sar(int a, int count) {
    int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "sarl %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

int test_rol(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "roll %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

int test_ror(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "rorl %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

long long test_add64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "addq %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

long long test_sub64(long long a, long long b) {
    long long result;
    __asm__ volatile (
        "subq %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_lea_simple(int a, int b) {
    int result;
    __asm__ volatile (
        "leal (%1, %2), %0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_lea_scaled(int a, int b) {
    int result;
    __asm__ volatile (
        "leal (%1, %2, 4), %0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
