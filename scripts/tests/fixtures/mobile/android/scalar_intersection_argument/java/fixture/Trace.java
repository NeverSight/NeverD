package fixture;
public final class Trace {
    private Trace() { }
    public static long events, number;
    public static int failAt, label, seenTag;
    public static boolean enabled;
    public static double fraction;
    public static Object receiver, value;
    public static Runnable action, failure;
    public static void hit(int step) {
        events = events * 10 + step;
        if (failAt == step) failure.run();
    }
}
