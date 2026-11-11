package fixture;
public final class Trace {
    private Trace() {}
    public static int events, calls, actions, reads, failAt, index;
    public static Object[] input;
    public static Object result;
    public static Runnable action;
    public static RuntimeException failure;
    public static Error error;
    public static void hit(int value) {
        events = events * 10 + value;
        if (failAt == value) throw failure;
    }
}
