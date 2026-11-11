package fixture;
public final class Trace {
    public static long events;
    public static int failAt, reads, inners, targets;
    public static boolean replace;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
