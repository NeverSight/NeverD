/* Flag manipulation, conditional operations, and extended test instructions */

int test_stc_clc(void) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        :: "al"
    );
    return result;
}

int test_cmc(void) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "cmc\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        :: "al"
    );
    return result;
}

int test_sahf_lahf(int flags) {
    int result;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "sahf\n\t"
        "lahf\n\t"
        "movzbl %%ah, %0"
        : "=r"(result)
        : "r"(flags)
        : "eax"
    );
    return result;
}

int test_test_and_jz(int a, int b) {
    int result;
    __asm__ volatile (
        "testl %2, %1\n\t"
        "setz %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_cmp_flags(int a, int b) {
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

int test_seta(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "seta %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_setge(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setge %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_seto(int a, int b) {
    int result;
    __asm__ volatile (
        "addl %2, %1\n\t"
        "seto %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_sets(int a) {
    int result;
    __asm__ volatile (
        "testl %1, %1\n\t"
        "sets %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a)
        : "al"
    );
    return result;
}

int test_setp(int a, int b) {
    int result;
    __asm__ volatile (
        "testl %2, %1\n\t"
        "setp %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_cmova(unsigned int a, unsigned int b, int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmoval %2, %0"
        : "=r"(result)
        : "0"(c), "r"(a), "r"(b)
    );
    return result;
}

int test_cmovl(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmovll %2, %0"
        : "=r"(result)
        : "0"(c), "r"(a), "r"(b)
    );
    return result;
}

int test_cmovs(int val, int a) {
    int result;
    __asm__ volatile (
        "testl %2, %2\n\t"
        "cmovsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
