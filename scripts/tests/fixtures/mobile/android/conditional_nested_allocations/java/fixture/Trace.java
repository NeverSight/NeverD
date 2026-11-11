package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Inner inner;
    public static Outer outer;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
