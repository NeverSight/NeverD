int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int switch_test(int cmd) {
    switch (cmd) {
    case 0: return 100;
    case 1: return 200;
    case 2: return 300;
    case 3: return 400;
    case 5: return 500;
    default: return -1;
    }
}

int nested_loop(int rows, int cols) {
    int sum = 0;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            sum += i * cols + j;
        }
    }
    return sum;
}

long bitops(long x, int shift) {
    long a = x & 0xFF00FF00;
    long b = (x >> shift) | (x << (64 - shift));
    return a ^ b;
}

void _start() {
    int r = fibonacci(10);
    r += switch_test(2);
    r += nested_loop(3, 4);
    r += (int)bitops(0xDEADBEEF, 13);
    asm volatile("mov $60, %%rax; mov %0, %%rdi; syscall" :: "r"((long)r) : "rax", "rdi");
}
