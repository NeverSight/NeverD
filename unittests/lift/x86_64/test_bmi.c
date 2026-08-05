/* BMI1/BMI2 and extended bit manipulation instruction tests */

unsigned int test_andn(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "andnl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

unsigned int test_bextr(unsigned int a, unsigned int ctrl) {
    unsigned int result;
    __asm__ volatile (
        "bextrl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(ctrl)
    );
    return result;
}

unsigned int test_blsi(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "blsil %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_blsmsk(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "blsmskl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_blsr(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "blsrl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_bzhi(unsigned int a, unsigned int idx) {
    unsigned int result;
    __asm__ volatile (
        "bzhil %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(idx)
    );
    return result;
}

unsigned int test_sarx(unsigned int a, unsigned int cnt) {
    unsigned int result;
    __asm__ volatile (
        "sarxl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

unsigned int test_shlx(unsigned int a, unsigned int cnt) {
    unsigned int result;
    __asm__ volatile (
        "shlxl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

unsigned int test_shrx(unsigned int a, unsigned int cnt) {
    unsigned int result;
    __asm__ volatile (
        "shrxl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(cnt)
    );
    return result;
}

unsigned int test_rorx(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "rorxl $7, %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_pdep(unsigned int a, unsigned int mask) {
    unsigned int result;
    __asm__ volatile (
        "pdepl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(mask)
    );
    return result;
}

unsigned int test_pext(unsigned int a, unsigned int mask) {
    unsigned int result;
    __asm__ volatile (
        "pextl %2, %1, %0"
        : "=r"(result)
        : "r"(a), "r"(mask)
    );
    return result;
}

unsigned int test_tzcnt(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "tzcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_lzcnt(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "lzcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_popcnt(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "popcntl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_crc32b(unsigned int crc, unsigned char data) {
    unsigned int result;
    __asm__ volatile (
        "crc32b %2, %0"
        : "=r"(result)
        : "0"(crc), "r"(data)
    );
    return result;
}

unsigned long long test_mulx(unsigned int a, unsigned int b) {
    unsigned int lo, hi;
    __asm__ volatile (
        "mulxl %3, %0, %1"
        : "=r"(lo), "=r"(hi)
        : "d"(a), "r"(b)
    );
    return ((unsigned long long)hi << 32) | lo;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
