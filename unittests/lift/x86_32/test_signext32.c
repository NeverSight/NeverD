/* x86-32 sign extension: CBW, CWDE, CDQ, CWD, MOVSX, MOVZX */

short test_cbw32(char val) {
    short result;
    __asm__ volatile (
        "movb %1, %%al\n\t"
        "cbw\n\t"
        "movw %%ax, %0"
        : "=r"(result)
        : "r"(val)
        : "ax"
    );
    return result;
}

int test_cwde32(short val) {
    int result;
    __asm__ volatile (
        "movw %1, %%ax\n\t"
        "cwde\n\t"
        "movl %%eax, %0"
        : "=r"(result)
        : "r"(val)
        : "eax"
    );
    return result;
}

int test_cdq32(int val) {
    int hi;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "cdq\n\t"
        "movl %%edx, %0"
        : "=r"(hi)
        : "r"(val)
        : "eax", "edx"
    );
    return hi;
}

int test_movsx_byte32(char val) {
    int result;
    __asm__ volatile (
        "movsbl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

unsigned int test_movzx_byte32(unsigned char val) {
    unsigned int result;
    __asm__ volatile (
        "movzbl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

int test_movsx_word32(short val) {
    int result;
    __asm__ volatile (
        "movswl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

unsigned int test_movzx_word32(unsigned short val) {
    unsigned int result;
    __asm__ volatile (
        "movzwl %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

void _start(void) {
    __asm__ volatile (
        "movl $1, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "int $0x80"
        ::: "eax", "ebx"
    );
}
