package fixture;
public final class Trace {
    public static long events;
    public static int observations, bad, failAt;
    public static Object seed, first, second;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
