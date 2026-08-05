#include <stdio.h>

int add(int a, int b) {
    return a + b;
}

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
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

int main() {
    int result = add(3, 4);
    printf("add(3,4) = %d\n", result);

    result = factorial(5);
    printf("factorial(5) = %d\n", result);

    int arr[] = {1, 2, 3, 4, 5};
    result = sum_array(arr, 5);
    printf("sum = %d\n", result);

    result = classify(42);
    printf("classify(42) = %d\n", result);
    return 0;
}
