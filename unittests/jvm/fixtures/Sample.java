public final class Sample {
    public static int arithmetic(int a, int b) {
        int sum = a + b;
        return sum * 3 - 1;
    }

    public static long distance(long left, long right) {
        return left - right;
    }

    public static int choose(int x) {
        if (x < 3) return x + 8;
        return x - 5;
    }

    public static int sumTo(int n) {
        int result = 0;
        while (n > 0) {
            result += n;
            n--;
        }
        return result;
    }

    public static int bucket(int x) {
        switch (x) {
        case -8: return 9;
        case 12: return 14;
        default: return 1;
        }
    }

    public static int divide(int n, int d) {
        try {
            return n / d;
        } catch (ArithmeticException error) {
            return -7;
        }
    }

    public static native long sampleNative(int value);
}
