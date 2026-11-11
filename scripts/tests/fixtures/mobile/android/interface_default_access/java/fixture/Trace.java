package fixture;
public final class Trace {
    public static long events;
    public static int failAt, overrides;
    public static RuntimeException failure;
    public static Object captured, current, returned, overrideResult;
    public static String tag;
    public static void hit(int value) {
        events = events * 10 + value;
        if (failAt == value) throw failure;
    }
}
