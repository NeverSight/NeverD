package fixture;
public final class Trace {
    public static int events, failAt;
    public static long flags;
    public static RuntimeException failure;
    public static Object saved, taskResult;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
