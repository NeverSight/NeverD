/* x86-32 control flow: Jcc, CALL, RET, CMOVcc, SETcc, LOOP */

int test_je32(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "je 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n"
        "1: movl $1, %0\n"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_jl32(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "jl 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n"
        "1: movl $1, %0\n"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_jb32(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "jb 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n"
        "1: movl $1, %0\n"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_sete32(int a, int b) {
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

int test_setl32(int a, int b) {
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

int test_cmove32(int a, int b, int sel) {
    int result;
    __asm__ volatile (
        "testl %3, %3\n\t"
        "cmovel %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b), "r"(sel)
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
