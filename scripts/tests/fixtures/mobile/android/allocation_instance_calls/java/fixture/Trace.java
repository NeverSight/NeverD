package fixture;
public final class Trace {
    public static int events, reads, failAt;
    public static Object result, alternate, seen;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
