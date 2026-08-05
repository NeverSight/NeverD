/* x86-64 atomic-like operations: CMPXCHG, XADD, XCHG */

int test_cmpxchg_match(int expected, int replacement) {
    int location = expected;
    int result;
    __asm__ volatile (
        "lock cmpxchgl %2, %1"
        : "=a"(result), "+m"(location)
        : "r"(replacement), "0"(expected)
    );
    return result;
}

int test_cmpxchg_nomatch(int expected, int replacement) {
    int location = expected + 1;
    int result;
    __asm__ volatile (
        "lock cmpxchgl %2, %1"
        : "=a"(result), "+m"(location)
        : "r"(replacement), "0"(expected)
    );
    return result;
}

int test_xadd(int a, int b) {
    __asm__ volatile (
        "xaddl %1, %0"
        : "+r"(a), "+r"(b)
    );
    return a;
}

int test_xchg(int a, int b) {
    __asm__ volatile (
        "xchgl %0, %1"
        : "+r"(a), "+r"(b)
    );
    return a;
}

long long test_xchg64(long long a, long long b) {
    __asm__ volatile (
        "xchgq %0, %1"
        : "+r"(a), "+r"(b)
    );
    return a;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
