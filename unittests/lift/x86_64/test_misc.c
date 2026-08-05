/* x86-64 misc: CLC, STC, CMC, CLD, STD, LAHF, SAHF, NOP, ENDBR, CPUID, RDTSC */

int test_clc_adc(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_stc_adc(int a, int b) {
    int result;
    __asm__ volatile (
        "stc\n\t"
        "adcl %2, %0"
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
        "adcl $0, %0"
        : "=r"(result)
        : "0"(0)
    );
    return result;
}

void test_nop(void) {
    __asm__ volatile ("nop");
}

void test_nop_long(void) {
    __asm__ volatile (
        "nop\n\t"
        "xchg %%ax, %%ax\n\t"
        ".byte 0x0f, 0x1f, 0x00\n\t"
        ".byte 0x0f, 0x1f, 0x40, 0x00"
        ::: "ax"
    );
}

unsigned int test_movzx_byte(unsigned char val) {
    unsigned int result;
    __asm__ volatile (
        "movzbl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

unsigned int test_movzx_word(unsigned short val) {
    unsigned int result;
    __asm__ volatile (
        "movzwl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_movsx_byte(char val) {
    int result;
    __asm__ volatile (
        "movsbl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_movsx_word(short val) {
    int result;
    __asm__ volatile (
        "movswl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
