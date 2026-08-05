/* x86-64 string operations: MOVS, STOS, CMPS, SCAS, REP prefixes */

void test_movsb(void* dst, const void* src, unsigned long count) {
    __asm__ volatile (
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

void test_movsq(void* dst, const void* src, unsigned long count) {
    __asm__ volatile (
        "rep movsq"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

void test_stosb(void* dst, unsigned char val, unsigned long count) {
    __asm__ volatile (
        "rep stosb"
        : "+D"(dst), "+c"(count)
        : "a"(val)
        : "memory"
    );
}

void test_stosq(void* dst, unsigned long long val, unsigned long count) {
    __asm__ volatile (
        "rep stosq"
        : "+D"(dst), "+c"(count)
        : "a"(val)
        : "memory"
    );
}

int test_cmpsb_single(const void* s1, const void* s2) {
    int result;
    __asm__ volatile (
        "cmpsb\n\t"
        "sete %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result), "+S"(s1), "+D"(s2)
        :
        : "al", "memory"
    );
    return result;
}

int test_scasb(const void* haystack, unsigned char needle) {
    int result;
    __asm__ volatile (
        "scasb\n\t"
        "sete %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result), "+D"(haystack)
        : "a"(needle)
        : "memory"
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
