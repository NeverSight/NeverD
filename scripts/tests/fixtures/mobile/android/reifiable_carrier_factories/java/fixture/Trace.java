package fixture;
public final class Trace {
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object input, result, after;
    public static java.util.Collection<?> collection, seen;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == 1 || event == 2) collection = java.util.Collections.emptyList();
        if (failAt == event) throw failure;
    }
}
