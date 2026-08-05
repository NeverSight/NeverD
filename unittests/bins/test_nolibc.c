int add(int a, int b) {
    return a + b;
}

int factorial(int n) {
    int result = 1;
    while (n > 1) {
        result *= n;
        n--;
    }
    return result;
}

int sum_array(int* arr, int len) {
    int total = 0;
    for (int i = 0; i < len; i++) {
        total += arr[i];
    }
    return total;
}

int classify(int x) {
    if (x < 0) return -1;
    else if (x == 0) return 0;
    else if (x < 100) return 1;
    else return 2;
}

void _start() {
    int arr[] = {1, 2, 3, 4, 5};
    int r = add(3, 4);
    r += factorial(5);
    r += sum_array(arr, 5);
    r += classify(42);
    // exit syscall
    asm volatile("mov $60, %%rax; mov %0, %%rdi; syscall" :: "r"((long)r) : "rax", "rdi");
}
