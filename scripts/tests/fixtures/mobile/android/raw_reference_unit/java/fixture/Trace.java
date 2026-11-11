package fixture;
public final class Trace {
    public static int events, calls, fail;
    public static Object receiver, first, second, third;
    public static Class<?> token;
    public static RuntimeException failure;
    public static void hit(int event) { events = events * 10 + event; if (fail == event) throw failure; }
}
