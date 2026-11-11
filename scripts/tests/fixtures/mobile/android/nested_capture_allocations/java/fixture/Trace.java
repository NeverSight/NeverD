package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static void step(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
