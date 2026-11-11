package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Object observed;
    public static int event(int value) {
        events = events * 10 + value;
        if (value == failAt) throw failure;
        return value;
    }
    public static int count() { event(1); return 17; }
    public static long wide() { event(2); return Long.MIN_VALUE + 11; }
    public static double number() { event(3); return Double.longBitsToDouble(0x7ff8000000000042L); }
}
