package fixture;
public final class Trace {
    public static int events, calls, constructors, failAt;
    public static RuntimeException failure;
    public static Holder holder;
    public static Object first, second, replaced, result, seen, after;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
