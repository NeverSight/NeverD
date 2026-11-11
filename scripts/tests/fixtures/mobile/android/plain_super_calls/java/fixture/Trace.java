package fixture;
public final class Trace {
    public static int events, failAt, code;
    public static Object receiver, box, token;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
