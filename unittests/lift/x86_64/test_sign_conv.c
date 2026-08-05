/* x86-64 sign extension / widening: CBW, CWDE, CDQE, CWD, CDQ, CQO, BSWAP */

long long test_cdqe(int val) {
    long long result;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "cdqe\n\t"
        "movq %%rax, %0"
        : "=r"(result)
        : "r"(val)
        : "rax"
    );
    return result;
}

int test_cdq(int val) {
    int result;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "cdq\n\t"
        "movl %%edx, %0"
        : "=r"(result)
        : "r"(val)
        : "eax", "edx"
    );
    return result;
}

long long test_cqo(long long val) {
    long long result;
    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "cqo\n\t"
        "movq %%rdx, %0"
        : "=r"(result)
        : "r"(val)
        : "rax", "rdx"
    );
    return result;
}

unsigned long long test_bswap64(unsigned long long val) {
    __asm__ volatile (
        "bswapq %0"
        : "+r"(val)
    );
    return val;
}

unsigned int test_bswap32(unsigned int val) {
    __asm__ volatile (
        "bswapl %0"
        : "+r"(val)
    );
    return val;
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
