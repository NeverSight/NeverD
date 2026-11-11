package fixture;
public final class Trace {
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object input, result, after;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
