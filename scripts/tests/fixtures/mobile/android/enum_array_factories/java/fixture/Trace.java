package fixture;
public final class Trace {
    public static int events, failAt, length;
    public static RuntimeException failure;
    public static Object seen, replacement, result, after;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
