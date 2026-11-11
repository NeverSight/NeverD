package fixture;
public final class Trace {
    public static java.util.List source; public static Config context;
    public static Object seenSource, seenContext; public static long snapshot;
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object input, result, after, key;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
