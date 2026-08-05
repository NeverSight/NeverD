/* x86-64 control flow and flag instruction semantics */

int test_cmp_je(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "je 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmp_jne(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "jne 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmp_jl(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "jl 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmp_jg(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "jg 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmp_jb(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "jb 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_cmp_ja(unsigned int a, unsigned int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "ja 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

int test_test_jz(int a) {
    int result;
    __asm__ volatile (
        "testl %1, %1\n\t"
        "jz 1f\n\t"
        "movl $0, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $1, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_cmov(int a, int b, int c) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "cmovgl %3, %0"
        : "=r"(result)
        : "r"(a), "r"(b), "r"(c), "0"(a)
    );
    return result;
}

int test_setcc(int a, int b) {
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

int test_movzx(short a) {
    int result;
    __asm__ volatile (
        "movzwl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_movsx(short a) {
    int result;
    __asm__ volatile (
        "movswl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_xchg(int a, int b) {
    __asm__ volatile (
        "xchgl %0, %1"
        : "+r"(a), "+r"(b)
    );
    return a;
}

int test_bswap(int a) {
    __asm__ volatile (
        "bswapl %0"
        : "+r"(a)
    );
    return a;
}

int test_bt(unsigned int a, int bit) {
    int result;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(bit)
        : "eax"
    );
    return result;
}

int test_bsr(unsigned int a) {
    int result;
    __asm__ volatile (
        "bsrl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_bsf(unsigned int a) {
    int result;
    __asm__ volatile (
        "bsfl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_popcnt(unsigned int a) {
    int result;
    __asm__ volatile (
        "popcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_lzcnt(unsigned int a) {
    int result;
    __asm__ volatile (
        "lzcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_tzcnt(unsigned int a) {
    int result;
    __asm__ volatile (
        "tzcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
