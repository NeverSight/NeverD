/* x86-64 FMA, ENTER/LEAVE, MOVBE, BSWAP, XLATB, LAHF/SAHF, CPUID, RDTSC. */

void test_enter_leave(void) {
    __asm__ volatile (
        "enter $16, $0\n\t"
        "nop\n\t"
        "leave"
    );
}

int test_bswap32(int a) {
    int result;
    __asm__ volatile (
        "bswapl %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

long long test_bswap64(long long a) {
    long long result;
    __asm__ volatile (
        "bswapq %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

void test_lahf_sahf(void) {
    __asm__ volatile (
        "xorl %%eax, %%eax\n\t"
        "lahf\n\t"
        "sahf"
        ::: "eax"
    );
}

void test_cpuid(void) {
    __asm__ volatile (
        "xorl %%eax, %%eax\n\t"
        "cpuid"
        ::: "eax", "ebx", "ecx", "edx"
    );
}

void test_rdtsc(void) {
    __asm__ volatile (
        "rdtsc"
        ::: "eax", "edx"
    );
}

void test_clflush(void) {
    int dummy;
    __asm__ volatile (
        "clflush (%0)"
        :
        : "r"(&dummy)
        : "memory"
    );
}

void test_fence(void) {
    __asm__ volatile (
        "mfence\n\t"
        "lfence\n\t"
        "sfence"
    );
}

int test_bt(int val, int bit) {
    int result = 0;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "adcl $0, %0"
        : "+r"(result)
        : "r"(val), "r"(bit)
    );
    return result;
}

int test_bts(int val, int bit) {
    int result;
    __asm__ volatile (
        "btsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

int test_btr(int val, int bit) {
    int result;
    __asm__ volatile (
        "btrl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

int test_btc(int val, int bit) {
    int result;
    __asm__ volatile (
        "btcl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

void test_cld_std(void) {
    __asm__ volatile (
        "cld\n\t"
        "std\n\t"
        "cld"
    );
}

void test_pause(void) {
    __asm__ volatile ("pause");
}

void test_int3(void) {
    __asm__ volatile ("int3");
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
