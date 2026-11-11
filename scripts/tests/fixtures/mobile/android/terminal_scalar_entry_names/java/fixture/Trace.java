package fixture;
public final class Trace {
    private Trace() {}
    public static int events;
    public static int failAt;
    public static boolean fatal;
    public static RuntimeException failure;
    public static Error error;
    public static void hit(int step) {
        events = events * 10 + step;
        if (step == failAt) {
            if (fatal) throw error;
            throw failure;
        }
    }
}
