/* x86-64 bit manipulation: BT, BTS, BTR, BTC, BSF, BSR, BSWAP, POPCNT, LZCNT, TZCNT */

int test_bt(unsigned int val, int bit) {
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

unsigned int test_bts(unsigned int val, int bit) {
    unsigned int result;
    __asm__ volatile (
        "btsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

unsigned int test_btr(unsigned int val, int bit) {
    unsigned int result;
    __asm__ volatile (
        "btrl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

unsigned int test_btc(unsigned int val, int bit) {
    unsigned int result;
    __asm__ volatile (
        "btcl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

int test_bsf(unsigned int val) {
    int result;
    __asm__ volatile (
        "bsfl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_bsr(unsigned int val) {
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

unsigned long long test_bswap64(unsigned long long val) {
    unsigned long long result;
    __asm__ volatile (
        "bswapq %0"
        : "=r"(result)
        : "0"(val)
    );
    return result;
}

int test_popcnt(unsigned int val) {
    int result;
    __asm__ volatile (
        "popcntl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_lzcnt(unsigned int val) {
    int result;
    __asm__ volatile (
        "lzcntl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_tzcnt(unsigned int val) {
    int result;
    __asm__ volatile (
        "tzcntl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
