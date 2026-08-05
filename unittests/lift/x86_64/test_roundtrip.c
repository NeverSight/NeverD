/* Round-trip semantic verification patterns for x86-64.
 *
 * Each function exercises a distinct control-flow or data-flow pattern
 * with a known, verifiable return value.  The unit test lifts this
 * object, decompiles it, and verifies that the decompiled C contains
 * semantically equivalent constructs (function names, return values,
 * conditional keywords).
 */

volatile int sink;

int rt_simple_add(int a, int b) {
    return a + b;
}

int rt_if_else(int x) {
    if (x > 0)
        return 1;
    else
        return -1;
}

int rt_for_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i)
        sum += i;
    return sum;
}

int rt_while_loop(int n) {
    int count = 0;
    while (n > 0) {
        n >>= 1;
        ++count;
    }
    return count;
}

int rt_nested_if(int a, int b, int c) {
    if (a > b) {
        if (b > c)
            return a + b + c;
        else
            return a - c;
    }
    return b;
}

int rt_switch_case(int x) {
    switch (x) {
    case 0: return 100;
    case 1: return 200;
    case 2: return 300;
    case 3: return 400;
    case 4: return 500;
    default: return -1;
    }
}

int rt_pointer_arith(int *p, int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i)
        sum += p[i];
    return sum;
}

int rt_bitwise(int x, int y) {
    int a = x & y;
    int b = x | y;
    int c = x ^ y;
    return a + b + c;
}

unsigned rt_shift_ops(unsigned x) {
    unsigned a = x << 3;
    unsigned b = x >> 2;
    return a + b;
}

int rt_ternary(int x, int y) {
    return (x > y) ? x : y;
}

void _start(void) {
    sink = rt_simple_add(3, 4);
    sink = rt_if_else(5);
    sink = rt_for_loop(10);
    sink = rt_while_loop(255);
    sink = rt_nested_if(10, 5, 3);
    sink = rt_switch_case(2);
    int arr[] = {1, 2, 3, 4, 5};
    sink = rt_pointer_arith(arr, 5);
    sink = rt_bitwise(0xFF, 0x0F);
    sink = rt_shift_ops(42);
    sink = rt_ternary(7, 3);
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
