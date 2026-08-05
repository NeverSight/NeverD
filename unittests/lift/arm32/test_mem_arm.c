/* ARM32 memory and load/store instruction tests */

int test_ldr_str(int *ptr) {
    int val;
    __asm__ volatile (
        "ldr %0, [%1]"
        : "=r"(val)
        : "r"(ptr)
        : "memory"
    );
    return val;
}

void test_str(int *ptr, int val) {
    __asm__ volatile (
        "str %1, [%0]"
        :
        : "r"(ptr), "r"(val)
        : "memory"
    );
}

int test_ldr_offset(int *ptr) {
    int val;
    __asm__ volatile (
        "ldr %0, [%1, #4]"
        : "=r"(val)
        : "r"(ptr)
        : "memory"
    );
    return val;
}

short test_ldrh(short *ptr) {
    short val;
    __asm__ volatile (
        "ldrh %0, [%1]"
        : "=r"(val)
        : "r"(ptr)
        : "memory"
    );
    return val;
}

char test_ldrb(char *ptr) {
    char val;
    __asm__ volatile (
        "ldrb %0, [%1]"
        : "=r"(val)
        : "r"(ptr)
        : "memory"
    );
    return val;
}

int test_ldm(int *ptr) {
    int a, b;
    __asm__ volatile (
        "ldm %2, {%0, %1}"
        : "=r"(a), "=r"(b)
        : "r"(ptr)
        : "memory"
    );
    return a + b;
}

int test_push_pop(int val) {
    int result;
    __asm__ volatile (
        "push {%1}\n\t"
        "pop {%0}"
        : "=r"(result)
        : "r"(val)
        : "memory"
    );
    return result;
}

void _start(void) {
    __asm__ volatile (
        "mov r7, #1\n\t"
        "mov r0, #0\n\t"
        "svc #0"
        ::: "r0", "r7"
    );
}
