/* x86-32 rotate/RCL/RCR instruction semantics via inline asm. */

unsigned int test_rol32(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "roll %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_ror32(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "rorl %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_rcl32(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "clc\n\t"
        "movl %2, %%ecx\n\t"
        "rcll %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_rcr32(unsigned int a, int count) {
    unsigned int result;
    __asm__ volatile (
        "clc\n\t"
        "movl %2, %%ecx\n\t"
        "rcrl %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_shld32(unsigned int hi, unsigned int lo, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %3, %%ecx\n\t"
        "shldl %%cl, %2, %0"
        : "=r"(result)
        : "0"(hi), "r"(lo), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_shrd32(unsigned int lo, unsigned int hi, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %3, %%ecx\n\t"
        "shrdl %%cl, %2, %0"
        : "=r"(result)
        : "0"(lo), "r"(hi), "r"(count)
        : "ecx"
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(0));
}
