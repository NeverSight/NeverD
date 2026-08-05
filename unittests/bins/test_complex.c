struct Point {
    int x, y;
};

int manhattan_distance(struct Point* a, struct Point* b) {
    int dx = a->x - b->x;
    int dy = a->y - b->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

int binary_search(int* arr, int len, int target) {
    int lo = 0, hi = len - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) return mid;
        if (arr[mid] < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int collatz_steps(int n) {
    int steps = 0;
    while (n != 1) {
        if (n & 1)
            n = 3 * n + 1;
        else
            n = n >> 1;
        steps++;
    }
    return steps;
}

typedef int (*binop_fn)(int, int);
int apply_op(binop_fn fn, int a, int b) { return fn(a, b); }
int add_fn(int a, int b) { return a + b; }
int mul_fn(int a, int b) { return a * b; }

int main() {
    struct Point p1 = {3, 7}, p2 = {10, 2};
    int d = manhattan_distance(&p1, &p2);

    int arr[] = {1, 3, 5, 7, 9, 11, 13};
    int idx = binary_search(arr, 7, 9);

    int steps = collatz_steps(27);

    int r1 = apply_op(add_fn, 10, 20);
    int r2 = apply_op(mul_fn, 5, 6);

    return d + idx + steps + r1 + r2;
}
