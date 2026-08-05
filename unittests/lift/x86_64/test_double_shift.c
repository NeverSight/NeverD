/* x86-64: SHLD/SHRD, RCL/RCR, ENTER/LEAVE, BSWAP64 */

unsigned long long test_shld(unsigned long long a, unsigned long long b) {
    unsigned long long result;
    __asm__ volatile (
        "shldq $8, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

unsigned long long test_shrd(unsigned long long a, unsigned long long b) {
    unsigned long long result;
    __asm__ volatile (
        "shrdq $8, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

unsigned int test_shld32(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "shldl $4, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

unsigned int test_shrd32(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "shrdl $4, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

unsigned int test_rcl(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "stc\n\t"
        "rcll $1, %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

unsigned int test_rcr(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "stc\n\t"
        "rcrl $1, %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

unsigned long long test_bswap64(unsigned long long a) {
    unsigned long long result;
    __asm__ volatile (
        "bswapq %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

unsigned int test_bswap32(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bswapl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

void test_enter_leave(void) {
    __asm__ volatile (
        "enter $16, $0\n\t"
        "leave"
        ::: "rbp", "rsp", "memory"
    );
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
