/* x86-64 loop instructions: LOOP, LOOPE, LOOPNE, ENTER/LEAVE */

int test_loop_sum(int n) {
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

int test_leave_frame(int val) {
    int result;
    __asm__ volatile (
        "pushq %%rbp\n\t"
        "movq %%rsp, %%rbp\n\t"
        "movl %1, %0\n\t"
        "leave"
        : "=r"(result)
        : "r"(val)
        : "rbp"
    );
    return result;
}

int test_bt(unsigned int val, int bit) {
    int result;
    __asm__ volatile (
        "btl %2, %1\n\t"
        "setc %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(val), "r"(bit)
        : "al"
    );
    return result;
}

int test_bts(unsigned int val, int bit) {
    unsigned int result;
    __asm__ volatile (
        "btsl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

int test_btr(unsigned int val, int bit) {
    unsigned int result;
    __asm__ volatile (
        "btrl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

int test_btc(unsigned int val, int bit) {
    unsigned int result;
    __asm__ volatile (
        "btcl %2, %0"
        : "=r"(result)
        : "0"(val), "r"(bit)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
