/* x86-32: enter/leave, bswap, cwd/cdq, loop, xchg, string ops */

unsigned int test_bswap32_misc(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bswapl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

void test_enter_leave32(void) {
    __asm__ volatile (
        "enter $8, $0\n\t"
        "leave"
        ::: "ebp", "esp", "memory"
    );
}

int test_cdq32(int a) {
    int hi;
    __asm__ volatile (
        "cdq"
        : "=d"(hi)
        : "a"(a)
    );
    return hi;
}

int test_cwd32(short a) {
    short hi;
    __asm__ volatile (
        "cwd"
        : "=d"(hi)
        : "a"(a)
    );
    return hi;
}

int test_cbw32(char a) {
    short result;
    __asm__ volatile (
        "cbw"
        : "=a"(result)
        : "a"(a)
    );
    return result;
}

int test_cwde32(short a) {
    int result;
    __asm__ volatile (
        "cwde"
        : "=a"(result)
        : "a"(a)
    );
    return result;
}

int test_xchg32(int a, int b) {
    int result;
    __asm__ volatile (
        "xchgl %0, %1"
        : "=r"(result), "+r"(b)
        : "0"(a)
    );
    return result;
}

int test_bsf32(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bsfl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_bsr32(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bsrl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

int test_cmpxchg32(int expected, int desired) {
    int result;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "cmpxchgl %2, %0"
        : "=r"(result)
        : "r"(expected), "r"(desired)
        : "eax"
    );
    return result;
}

int test_xadd32(int a, int b) {
    int result;
    __asm__ volatile (
        "xaddl %1, %0"
        : "=r"(result), "+r"(b)
        : "0"(a)
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
