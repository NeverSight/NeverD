/* x86-32 loop and sign extension: LOOP, LOOPE, LOOPNE, CBW, CWD, CWDE */

int test_loop_countdown(int n) {
    int result;
    __asm__ volatile (
        "xorl %0, %0\n\t"
        "1:\n\t"
        "addl $1, %0\n\t"
        "loop 1b"
        : "=r"(result)
        : "c"(n)
    );
    return result;
}

int test_cwde(short val) {
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

int test_cbw(char val) {
    int result;
    __asm__ volatile (
        "movb %1, %%al\n\t"
        "cbw\n\t"
        "movswl %%ax, %0"
        : "=r"(result)
        : "r"(val)
        : "eax"
    );
    return result;
}

int test_cwd(short val) {
    int result;
    __asm__ volatile (
        "movw %1, %%ax\n\t"
        "cwd\n\t"
        "movswl %%dx, %0"
        : "=r"(result)
        : "r"(val)
        : "eax", "edx"
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
