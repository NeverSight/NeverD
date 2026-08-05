/* x86-32 CMOVcc + SETcc instruction semantics via inline asm. */

int test_cmove32(int a, int b, int flag) {
    int result;
    __asm__ volatile (
        "cmpl $0, %2\n\t"
        "cmove %1, %0"
        : "=r"(result)
        : "r"(b), "r"(flag), "0"(a)
    );
    return result;
}

int test_cmovne32(int a, int b, int flag) {
    int result;
    __asm__ volatile (
        "cmpl $0, %2\n\t"
        "cmovne %1, %0"
        : "=r"(result)
        : "r"(b), "r"(flag), "0"(a)
    );
    return result;
}

int test_cmovl32(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmovl %1, %0"
        : "=r"(result)
        : "r"(b), "r"(a), "r"(c), "0"(a)
    );
    return result;
}

int test_cmovg32(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmovg %1, %0"
        : "=r"(result)
        : "r"(b), "r"(a), "r"(c), "0"(a)
    );
    return result;
}

int test_sete32(int a, int b) {
    int result = 0;
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
    int result = 0;
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

int test_cmovb32(unsigned int a, unsigned int b, unsigned int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmovb %1, %0"
        : "=r"(result)
        : "r"((int)b), "r"(a), "r"(c), "0"((int)a)
    );
    return result;
}

int test_cmova32(unsigned int a, unsigned int b, unsigned int c) {
    int result;
    __asm__ volatile (
        "cmpl %3, %2\n\t"
        "cmova %1, %0"
        : "=r"(result)
        : "r"((int)b), "r"(a), "r"(c), "0"((int)a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(0));
}
