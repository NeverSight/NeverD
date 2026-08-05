/* x86-64 sign extension: CBW, CWDE, CDQ, CQO, CDQE, CWD, MOVSXD */

short test_cbw(char val) {
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

long long test_cdqe(int val) {
    long long result;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "cdqe\n\t"
        "movq %%rax, %0"
        : "=r"(result)
        : "r"(val)
        : "rax"
    );
    return result;
}

int test_cdq(int val) {
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

long long test_cqo(long long val) {
    long long hi;
    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "cqo\n\t"
        "movq %%rdx, %0"
        : "=r"(hi)
        : "r"(val)
        : "rax", "rdx"
    );
    return hi;
}

long long test_movsxd(int val) {
    long long result;
    __asm__ volatile (
        "movslq %1, %0"
        : "=r"(result)
        : "r"(val)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
