/* x86-32 atomic-like operations: CMPXCHG, XADD, XCHG, BSWAP */

int test_xchg32(int a, int b) {
    __asm__ volatile (
        "xchgl %0, %1"
        : "+r"(a), "+r"(b)
    );
    return a;
}

unsigned int test_bswap32(unsigned int val) {
    __asm__ volatile (
        "bswapl %0"
        : "+r"(val)
    );
    return val;
}

int test_cmpxchg32_match(int* ptr, int expected, int desired) {
    int result;
    __asm__ volatile (
        "lock cmpxchgl %3, (%2)"
        : "=a"(result)
        : "0"(expected), "r"(ptr), "r"(desired)
        : "memory"
    );
    return result;
}

int test_xadd32(int* ptr, int val) {
    __asm__ volatile (
        "lock xaddl %0, (%1)"
        : "+r"(val)
        : "r"(ptr)
        : "memory"
    );
    return val;
}

void _start(void) {
    __asm__ volatile (
        "movl $1, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80"
        ::: "eax", "ebx"
    );
}
