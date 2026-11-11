package fixture;
public final class Trace {
    public static long events;
    public static int failAt, calls, overrides;
    public static Object receiver;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
