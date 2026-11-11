package fixture;
public final class Trace {
    public static int fail, events, reads, calls, observed;
    public static Provider input, seen;
    public static Base base;
    public static RuntimeException failure;
    public static void hit(int event) { events = events * 10 + event; if (fail == event) throw failure; }
}
