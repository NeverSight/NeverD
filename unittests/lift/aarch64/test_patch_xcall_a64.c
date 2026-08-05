/* Cross-function call patterns — AArch64 variant. */

volatile int sink;

static int helper_add(int a, int b) { return a + b; }
static int helper_mul(int a, int b) { return a * b; }

int compute_chain(int x) {
    int s = helper_add(x, 10);
    int m = helper_mul(s, 3);
    return helper_add(m, x);
}

int fib(int n) {
    if (n <= 1) return n;
    return helper_add(fib(n - 1), fib(n - 2));
}

int switch_with_calls(int x) {
    switch (x) {
    case 0: return helper_add(1, 2);
    case 1: return helper_mul(3, 4);
    case 2: return compute_chain(5);
    case 3: return fib(6);
    default: return helper_add(x, -1);
    }
}

void _start(void) {
    sink = compute_chain(5);
    sink = fib(8);
    sink = switch_with_calls(3);
    while(1) {}
}
