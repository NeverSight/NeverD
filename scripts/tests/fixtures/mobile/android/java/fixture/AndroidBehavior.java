package fixture;

public class AndroidBehavior {
    public static final String WORD = "neverd\0λ😀";
    public static final float NEGATIVE_ZERO = -0.0f;
    public static final double DOUBLE_NEGATIVE_ZERO = -0.0d;
    public static final long WIDE_CONSTANT = -9223372036854775807L;
    public static int seed;
    public static int initializationCount;
    static {
        seed = AndroidPeer.twice(9) + 3;
        initializationCount = initializationCount + 1;
    }

    private int value;
    public AndroidBehavior(int initial) { value = initial; }
    public int get() { return value; }
    public int add(int other) { return value + other; }
    public int exchange(int replacement) {
        int previous = value;
        value = replacement;
        return previous;
    }
    public static AndroidBehavior factory(int initial) { return new AndroidBehavior(initial); }
    public static int scalar(int left, int right) { return (left + right) * 3 - right; }
    public static int choose(int left, int right) { return left < right ? left : right; }
    public static int crossClass(int value) { return Nested.bump(AndroidPeer.twice(value)); }
    public static long wide(long left, long right) { return (left + right) ^ (left >>> 17); }
    public static float floatIdentity(float value) { return value; }
    public static double doubleIdentity(double value) { return value; }
    public static float floatAdd(float left, float right) { return left + right; }
    public static double doubleAdd(double left, double right) { return left + right; }
    public static double mixed(int narrow, long wide, float single, double value) {
        return narrow + wide + single + value;
    }
    public static int sum(int[] values, int count) {
        int total = 0;
        for (int index = 0; index < count; index++) total += values[index];
        return total;
    }
    public static int exchangeArray(int[] values, int index, int replacement) {
        int previous = values[index];
        values[index] = replacement;
        return previous;
    }
    public static int[] array() { return new int[]{1, -2, 2147483647, 4, 5, 6, 7, 8, 9, 10, 11, 12}; }
    public static int safeDivide(int left, int right) {
        try { return left / right; }
        catch (ArithmeticException failure) { return -1; }
    }
    public static int packed(int value) {
        switch (value) {
            case 1: return 4;
            case 2: return 8;
            case 3: return 9;
            default: return -1;
        }
    }
    public static int sparse(int value) {
        switch (value) {
            case -10000: return 7;
            case 100: return 8;
            case 333: return 9;
            default: return -1;
        }
    }

    public static class Nested {
        public static int bump(int value) { return value + 3; }
    }
}
