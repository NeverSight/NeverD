/* x86-32 string operations: MOVS, STOS, CMPS, SCAS */

void test_movsb32(void* dst, const void* src, unsigned int count) {
    __asm__ volatile (
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

void test_movsd32(void* dst, const void* src, unsigned int count) {
    __asm__ volatile (
        "rep movsl"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

void test_stosb32(void* dst, unsigned char val, unsigned int count) {
    __asm__ volatile (
        "rep stosb"
        : "+D"(dst), "+c"(count)
        : "a"(val)
        : "memory"
    );
}

void test_stosd32(void* dst, unsigned int val, unsigned int count) {
    __asm__ volatile (
        "rep stosl"
        : "+D"(dst), "+c"(count)
        : "a"(val)
        : "memory"
    );
}

void _start(void) {
    __asm__ volatile (
        "movl $1, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80"
        ::: "eax", "ebx"
    );
}
