/* x86-64 LODS, XLATB, PUSHF/POPF, CDQ/CWD/CBW conversions, MOVBE,
   CLD/STD, UD2 trap, and miscellaneous gap coverage. */

unsigned char test_lodsb(const void* src) {
    unsigned char result;
    __asm__ volatile (
        "lodsb"
        : "=a"(result), "+S"(src)
        :
        : "memory"
    );
    return result;
}

unsigned int test_lodsl(const void* src) {
    unsigned int result;
    __asm__ volatile (
        "lodsl"
        : "=a"(result), "+S"(src)
        :
        : "memory"
    );
    return result;
}

unsigned long long test_lodsq(const void* src) {
    unsigned long long result;
    __asm__ volatile (
        "lodsq"
        : "=a"(result), "+S"(src)
        :
        : "memory"
    );
    return result;
}

void test_pushf_popf(void) {
    __asm__ volatile (
        "pushfq\n\t"
        "popfq"
        ::: "memory"
    );
}

void test_cbw_cwde(void) {
    __asm__ volatile (
        "movb $0xFF, %%al\n\t"
        "cbw\n\t"
        "cwde\n\t"
        "cdqe"
        ::: "rax"
    );
}

int test_cwd(short a) {
    short lo;
    short hi;
    __asm__ volatile (
        "movw %2, %%ax\n\t"
        "cwd\n\t"
        "movw %%ax, %0\n\t"
        "movw %%dx, %1"
        : "=m"(lo), "=m"(hi)
        : "r"(a)
        : "ax", "dx"
    );
    return (int)hi;
}

int test_xlatb(const char* table, unsigned char idx) {
    unsigned char result;
    __asm__ volatile (
        "movb %2, %%al\n\t"
        "xlatb"
        : "=a"(result)
        : "b"(table), "r"(idx)
    );
    return result;
}

void test_cld_std_cld(void) {
    __asm__ volatile (
        "cld\n\t"
        "std\n\t"
        "cld"
    );
}

void test_stc_clc_cmc(void) {
    __asm__ volatile (
        "stc\n\t"
        "clc\n\t"
        "cmc\n\t"
        "stc\n\t"
        "cmc"
    );
}

int test_loop_insn(int count) {
    int result = 0;
    __asm__ volatile (
        "movl %1, %%ecx\n\t"
        "xorl %0, %0\n\t"
        "1:\n\t"
        "addl $1, %0\n\t"
        "loop 1b"
        : "=r"(result)
        : "r"(count)
        : "ecx"
    );
    return result;
}

void test_loope(void) {
    __asm__ volatile (
        "movl $5, %%ecx\n\t"
        "xorl %%eax, %%eax\n\t"
        "1:\n\t"
        "testl %%eax, %%eax\n\t"
        "loope 1b"
        ::: "eax", "ecx"
    );
}

void test_loopne(void) {
    __asm__ volatile (
        "movl $5, %%ecx\n\t"
        "movl $1, %%eax\n\t"
        "1:\n\t"
        "testl %%eax, %%eax\n\t"
        "loopne 1b"
        ::: "eax", "ecx"
    );
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
