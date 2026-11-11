package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static boolean allowNull;
    public static Object checked, last;
    public static RuntimeException failure, nullFailure;
    public static void event(int event) {
        events = events * 10 + event;
        if (event == failAt) throw failure;
    }
}
