/* Round-trip semantic verification patterns for ARM32 */

volatile int sink;

int rt_add_arm(int a, int b) { return a + b; }

int rt_branch_arm(int x) {
    if (x > 0) return 1;
    return -1;
}

int rt_loop_arm(int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += i;
    return s;
}

int rt_bitwise_arm(int x, int y) {
    return (x & y) + (x | y) + (x ^ y);
}

int rt_switch_arm(int x) {
    switch (x) {
    case 0: return 10;
    case 1: return 20;
    case 2: return 30;
    case 3: return 40;
    case 4: return 50;
    default: return -1;
    }
}

int rt_nested_arm(int a, int b) {
    if (a > 0) {
        if (b > 0) return a + b;
        return a - b;
    }
    return b;
}

unsigned rt_shift_arm(unsigned x) {
    return (x << 3) + (x >> 2);
}

void _start(void) {
    sink = rt_add_arm(3, 4);
    sink = rt_branch_arm(5);
    sink = rt_loop_arm(10);
    sink = rt_bitwise_arm(0xFF, 0x0F);
    sink = rt_switch_arm(2);
    sink = rt_nested_arm(5, 3);
    sink = rt_shift_arm(42);
}
