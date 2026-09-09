package fixture;

public final class LocalClassBehavior {
    public static final int CONSTANT = 7;
    public static int firstCount;
    public static int secondCount;
    public static int wideCount;

    private LocalClassBehavior() {}

    public static int first(int value, int limit) {
        final class Worker {
            Worker() { firstCount = firstCount + 1; }

            int apply(int input, int count) {
                int result = input;
                int bound = count & 7;
                for (int i = 0; i < bound; i++) {
                    if ((result & 1) == 0) result += 3;
                    else result -= 5;
                }
                try { return result / (count - 1); }
                catch (ArithmeticException failure) { return result ^ 0x13579bdf; }
            }
        }
        return new Worker().apply(value, limit);
    }

    public static int second(int value, int divisor) {
        final class Worker {
            Worker() { secondCount = secondCount + 1; }

            int apply(int input, int denominator) {
                try {
                    int quotient = input / denominator;
                    if (quotient < 0) return quotient * 3 - 7;
                    return quotient + 11;
                } catch (ArithmeticException failure) {
                    return input ^ 0x02468ace;
                }
            }
        }
        return new Worker().apply(value, divisor);
    }

    public static long first(long value) {
        final class Worker {
            Worker() { wideCount = wideCount + 1; }

            long apply(long input) {
                return (input << 3) + (input >>> 61) - 17L;
            }
        }
        return new Worker().apply(value);
    }
}
