/* Memory, atomic, and address manipulation instruction tests */

int test_xadd(int *ptr, int val) {
    int result;
    __asm__ volatile (
        "lock xaddl %0, (%1)"
        : "=r"(result)
        : "r"(ptr), "0"(val)
        : "memory"
    );
    return result;
}

int test_cmpxchg_success(int *ptr, int expected, int desired) {
    int result;
    __asm__ volatile (
        "lock cmpxchgl %3, (%1)"
        : "=a"(result)
        : "r"(ptr), "a"(expected), "r"(desired)
        : "memory"
    );
    return result;
}

void test_lock_add(int *ptr, int val) {
    __asm__ volatile (
        "lock addl %1, (%0)"
        :
        : "r"(ptr), "r"(val)
        : "memory"
    );
}

void test_lock_inc(int *ptr) {
    __asm__ volatile (
        "lock incl (%0)"
        :
        : "r"(ptr)
        : "memory"
    );
}

void test_lock_dec(int *ptr) {
    __asm__ volatile (
        "lock decl (%0)"
        :
        : "r"(ptr)
        : "memory"
    );
}

void test_lock_or(int *ptr, int val) {
    __asm__ volatile (
        "lock orl %1, (%0)"
        :
        : "r"(ptr), "r"(val)
        : "memory"
    );
}

int test_lea_complex(long a, long b) {
    long result;
    __asm__ volatile (
        "leaq (%1, %2, 8), %0"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return (int)result;
}

int test_lea_rip_relative(void) {
    long result;
    __asm__ volatile (
        "leaq (%%rip), %0"
        : "=r"(result)
    );
    return (int)result;
}

void test_prefetch(void *ptr) {
    __asm__ volatile (
        "prefetcht0 (%0)"
        :
        : "r"(ptr)
    );
}

void test_clflush(void *ptr) {
    __asm__ volatile (
        "clflush (%0)"
        :
        : "r"(ptr)
        : "memory"
    );
}

void test_nop_variants(void) {
    __asm__ volatile (
        "nop\n\t"
        "nopl (%%rax)\n\t"
        "nopw (%%rax)\n\t"
        ::
        : "rax"
    );
}

long test_movabs(void) {
    long result;
    __asm__ volatile (
        "movabsq $0x123456789ABCDEF0, %0"
        : "=r"(result)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
