/* x86-64 RCL/RCR/LOOP/ENTER instruction semantics via inline asm. */

unsigned int test_rcl(unsigned int a, int count) {
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

unsigned int test_rcr(unsigned int a, int count) {
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

unsigned long long test_rcl64(unsigned long long a, int count) {
    unsigned long long result;
    __asm__ volatile (
        "clc\n\t"
        "movl %2, %%ecx\n\t"
        "rclq %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

unsigned long long test_rcr64(unsigned long long a, int count) {
    unsigned long long result;
    __asm__ volatile (
        "clc\n\t"
        "movl %2, %%ecx\n\t"
        "rcrq %%cl, %0"
        : "=r"(result)
        : "0"(a), "r"(count)
        : "ecx"
    );
    return result;
}

int test_loop_sum(int n) {
    int sum;
    __asm__ volatile (
        "xorl %%eax, %%eax\n\t"
        "movl %1, %%ecx\n\t"
        "1: addl %%ecx, %%eax\n\t"
        "loop 1b\n\t"
        "movl %%eax, %0"
        : "=r"(sum)
        : "r"(n)
        : "eax", "ecx"
    );
    return sum;
}

int test_enter_leave(int a) {
    int result;
    __asm__ volatile (
        "enter $0, $0\n\t"
        "movl %1, %%eax\n\t"
        "addl $1, %%eax\n\t"
        "movl %%eax, %0\n\t"
        "leave"
        : "=r"(result)
        : "r"(a)
        : "eax"
    );
    return result;
}

int test_sbb_chain(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "sbbl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

int test_adc_chain(int a, int b) {
    int result;
    __asm__ volatile (
        "clc\n\t"
        "adcl %2, %0"
        : "=r"(result)
        : "0"(a), "r"(b)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
