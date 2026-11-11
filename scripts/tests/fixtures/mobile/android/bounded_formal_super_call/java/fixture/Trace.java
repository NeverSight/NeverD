package fixture;
public final class Trace {
    public static int failAt, bits;
    public static long events;
    public static boolean result;
    public static Object receiver, context, value, after;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
