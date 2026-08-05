/* x86-64: flag manipulation, SAHF/LAHF, CLC/STC/CMC, CLD/STD, LOOP */

unsigned char test_lahf(int a, int b) {
    unsigned char result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "lahf\n\t"
        "movb %%ah, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "ah"
    );
    return result;
}

int test_sahf_then_jcc(int a, int b) {
    int result;
    __asm__ volatile (
        "cmpl %2, %1\n\t"
        "lahf\n\t"
        "xorl %%eax, %%eax\n\t"
        "sahf\n\t"
        "je 1f\n\t"
        "movl $1, %0\n\t"
        "jmp 2f\n\t"
        "1: movl $0, %0\n\t"
        "2:"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "eax"
    );
    return result;
}

int test_clc_adc(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_stc_adc(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cmc(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "cmc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_bt(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(val), "r"(bit_idx)
        : "eax"
    );
    return result;
}

int test_bts(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit_idx)
    );
    return result;
}

int test_btr(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btrl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit_idx)
    );
    return result;
}

int test_btc(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btcl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit_idx)
    );
    return result;
}

int test_tzcnt(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "tzcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_lzcnt(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "lzcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_popcnt(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "popcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
