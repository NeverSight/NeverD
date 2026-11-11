package fixture;
public final class Trace {
    public static long events, flags;
    public static int failAt;
    public static RuntimeException failure;
    public static Port<?> port;
    public static Sink<?> sink;
    public static Box<?> box;
    public static Object callbackResult;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
