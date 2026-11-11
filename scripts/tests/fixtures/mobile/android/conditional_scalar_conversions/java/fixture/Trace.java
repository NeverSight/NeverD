package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static float value;
    public static int tail;
    public static Object seen;
    public static int reads;
    public static void event(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
