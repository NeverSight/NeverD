/* ADX, extended arithmetic, and miscellaneous x86_64 instruction tests */

unsigned int test_adcx(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "stc\n\t"
        "adcxl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

unsigned int test_adox(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile (
        "adoxl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
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

unsigned long long test_bswap64(unsigned long long a) {
    unsigned long long result;
    __asm__ volatile (
        "bswapq %0"
        : "=r"(result)
        : "0"(a)
    );
    return result;
}

unsigned int test_movbe_load(unsigned int *ptr) {
    unsigned int result;
    __asm__ volatile (
        "movbe (%1), %0"
        : "=r"(result)
        : "r"(ptr)
        : "memory"
    );
    return result;
}

void test_movbe_store(unsigned int *ptr, unsigned int val) {
    __asm__ volatile (
        "movbe %1, (%0)"
        :
        : "r"(ptr), "r"(val)
        : "memory"
    );
}

unsigned int test_xchg(unsigned int a, unsigned int b) {
    unsigned int r1, r2;
    __asm__ volatile (
        "xchgl %0, %1"
        : "=r"(r1), "=r"(r2)
        : "0"(a), "1"(b)
    );
    return r1;
}

unsigned long long test_rdtsc(void) {
    unsigned int lo, hi;
    __asm__ volatile (
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );
    return ((unsigned long long)hi << 32) | lo;
}

void test_cpuid(unsigned int leaf, unsigned int *eax, unsigned int *ebx,
                unsigned int *ecx, unsigned int *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0)
    );
}

void test_mfence(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

void test_lfence(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

void test_sfence(void) {
    __asm__ volatile ("sfence" ::: "memory");
}

void test_pause(void) {
    __asm__ volatile ("pause");
}

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

unsigned int test_bsf(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bsfl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

unsigned int test_bsr(unsigned int a) {
    unsigned int result;
    __asm__ volatile (
        "bsrl %1, %0"
        : "=r"(result)
        : "r"(a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
