/* Round-trip semantic verification patterns for AArch64 */

volatile int sink;

int rt_add_a64(int a, int b) { return a + b; }

int rt_branch_a64(int x) {
    if (x > 0) return 1;
    return -1;
}

int rt_loop_a64(int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += i;
    return s;
}

int rt_bitwise_a64(int x, int y) {
    return (x & y) + (x | y) + (x ^ y);
}

int rt_switch_a64(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    default: return -1;
    }
}

int rt_nested_a64(int a, int b) {
    if (a > 0) {
        if (b > 0) return a + b;
        return a - b;
    }
    return b;
}

unsigned rt_shift_a64(unsigned x) {
    return (x << 3) + (x >> 2);
}

void _start(void) {
    sink = rt_add_a64(3, 4);
    sink = rt_branch_a64(5);
    sink = rt_loop_a64(10);
    sink = rt_bitwise_a64(0xFF, 0x0F);
    sink = rt_switch_a64(2);
    sink = rt_nested_a64(5, 3);
    sink = rt_shift_a64(42);
}
