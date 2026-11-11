package fixture;
public final class Trace {
    public static long events;
    public static int failAt, overrides, calls;
    public static Object input, value, receiver;
    public static RuntimeException failure;
    public static void hit(int value) {
        events = events * 10 + value;
        if (value == failAt) throw failure;
    }
}
