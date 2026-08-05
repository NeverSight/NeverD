/* x86-32: flag manipulation, BT/BTS/BTR/BTC, CLC/STC/CMC, SAHF/LAHF */

int test_clc_adc32(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_stc_adc32(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cmc32(int a, int b) {
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

int test_bt32(int val, int bit_idx) {
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

int test_bts32(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit_idx)
    );
    return result;
}

int test_btr32(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btrl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit_idx)
    );
    return result;
}

int test_btc32(int val, int bit_idx) {
    int result;
    __asm__ volatile (
        "btcl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit_idx)
    );
    return result;
}

int test_rol32(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "roll $4, %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_ror32(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rorl $4, %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

int test_shld32(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "shldl $4, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_shrd32(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "shrdl $4, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

unsigned char test_lahf32(int a, int b) {
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

void _start(void) {
    __asm__ volatile (
        "movl $1, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80"
        ::: "eax", "ebx"
    );
}
