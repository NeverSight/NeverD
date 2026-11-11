package fixture;
public final class Trace {
    public static int failAt, code;
    public static Context first, second;
    public static long events;
    public static Object input, result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
