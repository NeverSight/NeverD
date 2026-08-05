/* x86-64 stack and data movement instruction semantics */

long long test_push_pop(long long val) {
    long long result;
    __asm__ volatile (
        "pushq %1\n\t"
        "popq %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_mov_imm(void) {
    int result;
    __asm__ volatile (
        "movl $42, %0"
        : "=r"(result)
    );
    return result;
}

long long test_movabs(void) {
    long long result;
    __asm__ volatile (
        "movabsq $0x123456789ABCDEF0, %0"
        : "=r"(result)
    );
    return result;
}

int test_cbw(signed char a) {
    int result;
    __asm__ volatile (
        "movb %1, %%al\n\t"
        "cbw\n\t"
        "movswl %%ax, %0"
        : "=r"(result)
        : "r"(a)
        : "eax"
    );
    return result;
}

int test_cwde(short a) {
    int result;
    __asm__ volatile (
        "movw %1, %%ax\n\t"
        "cwde\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(a)
        : "eax"
    );
    return result;
}

int test_adc(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_sbb(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_cmc(void) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "cmc\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        :
        : "eax"
    );
    return result;
}

int test_stc_clc(void) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "setc %%al\n\t"
        "clc\n\t"
        "setc %%cl\n\t"
        "subb %%cl, %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        :
        : "eax", "ecx"
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
