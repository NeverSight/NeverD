package fixture;
public final class Trace {
    public static int events, calls, fail, unboxes, mask;
    public static Object receiver, first, second;
    public static RuntimeException failure;
    public static void hit(int event) { events = events * 10 + event; if (fail == event) throw failure; }
}
