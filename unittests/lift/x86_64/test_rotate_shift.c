/* x86-64 rotate/shift variants: ROL, ROR, RCL, RCR, SHLD, SHRD */

unsigned int test_rol(unsigned int val, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "roll %%cl, %0"
        : "=r"(result)
        : "0"(val), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_ror(unsigned int val, int count) {
    unsigned int result;
    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "rorl %%cl, %0"
        : "=r"(result)
        : "0"(val), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_rcl(unsigned int val, int count) {
    unsigned int result;
    __asm__ volatile (
        "clc\n\t"
        "movl %2, %%ecx\n\t"
        "rcll %%cl, %0"
        : "=r"(result)
        : "0"(val), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_rcr(unsigned int val, int count) {
    unsigned int result;
    __asm__ volatile (
        "clc\n\t"
        "movl %2, %%ecx\n\t"
        "rcrl %%cl, %0"
        : "=r"(result)
        : "0"(val), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned long long test_shld(unsigned long long hi, unsigned long long lo, int count) {
    unsigned long long result;
    __asm__ volatile (
        "movl %3, %%ecx\n\t"
        "shldq %%cl, %2, %0"
        : "=r"(result)
        : "0"(hi), "r"(lo), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned long long test_shrd(unsigned long long lo, unsigned long long hi, int count) {
    unsigned long long result;
    __asm__ volatile (
        "movl %3, %%ecx\n\t"
        "shrdq %%cl, %2, %0"
        : "=r"(result)
        : "0"(lo), "r"(hi), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned int test_rol_imm(unsigned int val) {
    unsigned int result;
    __asm__ volatile (
        "roll $5, %0"
        : "=r"(result)
        : "0"(val)
    );
    return result;
}

unsigned int test_ror_imm(unsigned int val) {
    unsigned int result;
    __asm__ volatile (
        "rorl $5, %0"
        : "=r"(result)
        : "0"(val)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
