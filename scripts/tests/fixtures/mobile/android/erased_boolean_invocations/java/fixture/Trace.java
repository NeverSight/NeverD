package fixture;
public final class Trace {
    public static long events, flags, bits;
    public static int failAt, calls, reads, accepts;
    public static Object value;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
