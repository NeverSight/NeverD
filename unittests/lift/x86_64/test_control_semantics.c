/* Control flow semantic preservation tests for x86-64.
   Tests nested if/else, switch-like patterns, and loops
   to verify HighIR structuring preserves all branches. */

int test_nested_if_else(int x) {
    int result;
    __asm__ volatile (
        "cmpl $0, %1\n\t"
        "jl .Lneg_%=\n\t"
        "je .Lzero_%=\n\t"
        "cmpl $100, %1\n\t"
        "jge .Lbig_%=\n\t"
        "movl $1, %0\n\t"
        "jmp .Ldone_%=\n"
        ".Lbig_%=:\n\t"
        "movl $2, %0\n\t"
        "jmp .Ldone_%=\n"
        ".Lzero_%=:\n\t"
        "movl $0, %0\n\t"
        "jmp .Ldone_%=\n"
        ".Lneg_%=:\n\t"
        "movl $-1, %0\n"
        ".Ldone_%=:"
        : "=r"(result) : "r"(x)
    );
    return result;
}

int test_loop_accumulate(int n) {
    int result;
    __asm__ volatile (
        "xorl %0, %0\n\t"
        "testl %1, %1\n\t"
        "jle .Lend_%=\n"
        ".Lloop_%=:\n\t"
        "addl %1, %0\n\t"
        "decl %1\n\t"
        "jnz .Lloop_%=\n"
        ".Lend_%=:"
        : "=r"(result) : "r"(n) : "cc"
    );
    return result;
}

int test_conditional_move(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "cmovgl %1, %0\n\t"
        "cmovlel %2, %0"
        : "=r"(result) : "r"(a), "r"(b) : "cc"
    );
    return result;
}

int test_adc_chain(int a, int b) {
    int lo, hi;
    __asm__ volatile (
        "addl %3, %0\n\t"
        "adcl $0, %1"
        : "=r"(lo), "=r"(hi) : "0"(a), "r"(b), "1"(0) : "cc"
    );
    return hi;
}

unsigned int test_bswap32(unsigned int x) {
    unsigned int result;
    __asm__ volatile (
        "bswapl %0"
        : "=r"(result) : "0"(x)
    );
    return result;
}

unsigned long long test_bswap64(unsigned long long x) {
    unsigned long long result;
    __asm__ volatile (
        "bswapq %0"
        : "=r"(result) : "0"(x)
    );
    return result;
}

int test_bt(unsigned int val, int bit) {
    int result;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result) : "r"(val), "r"(bit) : "al", "cc"
    );
    return result;
}

int test_bsf(unsigned int val) {
    int result;
    __asm__ volatile (
        "bsfl %1, %0"
        : "=r"(result) : "r"(val) : "cc"
    );
    return result;
}

int test_popcnt(unsigned int val) {
    int result;
    __asm__ volatile (
        "popcntl %1, %0"
        : "=r"(result) : "r"(val) : "cc"
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
