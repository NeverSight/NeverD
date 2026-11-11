package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static void event(int value) {
        events = events * 10 + value;
        if (value == failAt) throw failure;
    }
}
