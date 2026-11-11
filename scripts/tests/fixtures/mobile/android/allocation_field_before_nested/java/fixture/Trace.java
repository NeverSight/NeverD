package fixture;
public final class Trace {
    public static Holder holder;
    public static Object replacement;
    public static Outer result;
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
