/* i386 LODS, PUSHF/POPF, CWD/CBW, STC/CLC/CMC, LOOP variants */

unsigned char test_lodsb32(const void* src) {
    unsigned char result;
    __asm__ volatile (
        "lodsb"
        : "=a"(result), "+S"(src)
        :
        : "memory"
    );
    return result;
}

unsigned int test_lodsl32(const void* src) {
    unsigned int result;
    __asm__ volatile (
        "lodsl"
        : "=a"(result), "+S"(src)
        :
        : "memory"
    );
    return result;
}

void test_pushf_popf32(void) {
    __asm__ volatile (
        "pushfl\n\t"
        "popfl"
        ::: "memory"
    );
}

void test_cbw_cwde32(void) {
    __asm__ volatile (
        "movb $0xFF, %%al\n\t"
        "cbw\n\t"
        "cwde"
        ::: "eax"
    );
}

int test_cwd32(short a) {
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

void test_stc_clc_cmc32(void) {
    __asm__ volatile (
        "stc\n\t"
        "clc\n\t"
        "cmc\n\t"
        "stc\n\t"
        "cmc"
    );
}

int test_loop32(int count) {
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

void test_loope32(void) {
    __asm__ volatile (
        "movl $5, %%ecx\n\t"
        "xorl %%eax, %%eax\n\t"
        "1:\n\t"
        "testl %%eax, %%eax\n\t"
        "loope 1b"
        ::: "eax", "ecx"
    );
}

void test_loopne32(void) {
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
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(0));
}
