package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Carrier<?> parent, other, created;
    public static Object extracted, captured;
    public static String label;
    public static void hit(int value) {
        events = events * 10 + value;
        if (failAt == value) throw failure;
    }
}
