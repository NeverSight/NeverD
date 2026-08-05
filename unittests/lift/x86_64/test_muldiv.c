/* x86-64 multiply/divide instruction semantics */

int test_imul_2op(int a, int b) {
    int result;
    __asm__ volatile (
        "imull %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_imul_3op(int a, int b) {
    int result;
    __asm__ volatile (
        "imull %2, %1, %0"
        : "=r"(result)
        : "r"(a), "i"(7)
    );
    return result;
}

/* 1-operand MUL: EDX:EAX = EAX * r/m32 (unsigned) */
unsigned long long test_mul_1op(unsigned int a, unsigned int b) {
    unsigned int lo, hi;
    __asm__ volatile (
        "mull %2"
        : "=a"(lo), "=d"(hi)
        : "r"(b), "0"(a)
    );
    return ((unsigned long long)hi << 32) | lo;
}

/* 1-operand IMUL: EDX:EAX = EAX * r/m32 (signed) */
long long test_imul_1op(int a, int b) {
    int lo, hi;
    __asm__ volatile (
        "imull %2"
        : "=a"(lo), "=d"(hi)
        : "r"(b), "0"(a)
    );
    return ((long long)hi << 32) | (unsigned int)lo;
}

/* DIV: EAX = EDX:EAX / r/m32, EDX = EDX:EAX mod r/m32 (unsigned) */
unsigned int test_div(unsigned int dividend, unsigned int divisor) {
    unsigned int quot;
    __asm__ volatile (
        "xorl %%edx, %%edx\n\t"
        "divl %2"
        : "=a"(quot)
        : "0"(dividend), "r"(divisor)
        : "edx"
    );
    return quot;
}

unsigned int test_div_remainder(unsigned int dividend, unsigned int divisor) {
    unsigned int rem;
    __asm__ volatile (
        "xorl %%edx, %%edx\n\t"
        "divl %2"
        : "=d"(rem)
        : "a"(dividend), "r"(divisor)
    );
    return rem;
}

/* IDIV: EAX = EDX:EAX / r/m32, EDX = EDX:EAX mod r/m32 (signed) */
int test_idiv(int dividend, int divisor) {
    int quot;
    __asm__ volatile (
        "cdq\n\t"
        "idivl %2"
        : "=a"(quot)
        : "0"(dividend), "r"(divisor)
        : "edx"
    );
    return quot;
}

int test_idiv_remainder(int dividend, int divisor) {
    int rem;
    __asm__ volatile (
        "cdq\n\t"
        "idivl %2"
        : "=d"(rem)
        : "a"(dividend), "r"(divisor)
    );
    return rem;
}

/* CDQ: sign-extend EAX into EDX:EAX */
long long test_cdq(int a) {
    int lo, hi;
    __asm__ volatile (
        "cdq"
        : "=a"(lo), "=d"(hi)
        : "0"(a)
    );
    return ((long long)hi << 32) | (unsigned int)lo;
}

/* CDQE: sign-extend EAX into RAX (64-bit) */
long long test_cdqe(int a) {
    long long result;
    __asm__ volatile (
        "cdqe"
        : "=a"(result)
        : "0"((long long)a)
    );
    return result;
}

/* CQO: sign-extend RAX into RDX:RAX */
long long test_cqo_hi(long long a) {
    long long hi;
    __asm__ volatile (
        "cqo"
        : "=d"(hi)
        : "a"(a)
    );
    return hi;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
