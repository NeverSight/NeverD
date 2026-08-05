/* x86-64 conditional operations: SETcc, CMOVcc */

int test_sete(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "sete %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_setne(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setne %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_setl(int a, int b) {
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

int test_setg(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setg %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_setb(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "setb %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_seta(int a, int b) {
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

int test_sets(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "sets %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "al"
    );
    return result;
}

int test_cmove(int a, int b, int sel) {
    int result;
    __asm__ volatile (
        "testl %3, %3\n\t"
        "cmovel %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b), "r"(sel)
    );
    return result;
}

int test_cmovne(int a, int b, int sel) {
    int result;
    __asm__ volatile (
        "testl %3, %3\n\t"
        "cmovnel %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b), "r"(sel)
    );
    return result;
}

int test_cmovl(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "cmovll %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cmovg(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "cmovgl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cmovb(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "cmovbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cmova(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "cmoval %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
