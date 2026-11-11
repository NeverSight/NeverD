package fixture;
public final class Trace {
    public static int events, failAt;
    public static RuntimeException failure;
    public static Object seenToken;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
