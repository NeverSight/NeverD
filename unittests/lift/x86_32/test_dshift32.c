/* i386 double-shift, ENTER/LEAVE, BSWAP, BT family, flag manipulation. */

int test_shld(int a, int b, int count) {
    int result;
    __asm__ volatile (
        "movl %3, %%ecx\n\t"
        "shldl %%cl, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b), "r"(count)
        : "ecx"
    );
    return result;
}

int test_shrd(int a, int b, int count) {
    int result;
    __asm__ volatile (
        "movl %3, %%ecx\n\t"
        "shrdl %%cl, %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b), "r"(count)
        : "ecx"
    );
    return result;
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

void test_enter_leave(void) {
    __asm__ volatile (
        "enter $8, $0\n\t"
        "nop\n\t"
        "leave"
    );
}

int test_bt32(int val, int bit) {
    int result = 0;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "adcl $0, %0"
        : "+r"(result)
        : "r"(val), "r"(bit)
    );
    return result;
}

int test_bts32(int val, int bit) {
    int result;
    __asm__ volatile (
        "btsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

int test_btr32(int val, int bit) {
    int result;
    __asm__ volatile (
        "btrl %2, %0"
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

void test_lahf(void) {
    __asm__ volatile (
        "xorl %%eax, %%eax\n\t"
        "lahf"
        ::: "eax"
    );
}

int test_xchg32(int a, int b) {
    int ra = a, rb = b;
    __asm__ volatile (
        "xchgl %0, %1"
        : "+r"(ra), "+r"(rb)
    );
    return ra;
}

void _start(void) {
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(0));
}
