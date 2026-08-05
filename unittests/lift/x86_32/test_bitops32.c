/* x86-32 bit ops: ADC, SBB, BT, BSF, BSR, BSWAP, XCHG, CMPXCHG */

int test_adc32(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb32(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_bt32(unsigned int val, int bit) {
    int result;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(val), "r"(bit)
        : "al"
    );
    return result;
}

int test_bsf32(unsigned int val) {
    int result;
    __asm__ volatile (
        "bsfl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_bsr32(unsigned int val) {
    int result;
    __asm__ volatile (
        "bsrl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

unsigned int test_bswap32(unsigned int val) {
    unsigned int result;
    __asm__ volatile (
        "bswapl %0"
        : "=r"(result)
        : "0"(val)
    );
    return result;
}

int test_xchg32(int a, int b) {
    __asm__ volatile (
        "xchgl %0, %1"
        : "+r"(a), "+r"(b)
    );
    return a;
}

unsigned int test_rol32(unsigned int val, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "roll %%cl, %0"
        : "=r"(result)
        : "0"(val), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_ror32(unsigned int val, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "rorl %%cl, %0"
        : "=r"(result)
        : "0"(val), "r"(count)
        : "ecx"
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
