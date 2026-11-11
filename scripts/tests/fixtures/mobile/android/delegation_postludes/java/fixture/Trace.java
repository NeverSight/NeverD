package fixture;
public final class Trace {
    public static int failAt;
    public static RuntimeException failure;
    public static long events;
    public static Core current;
    public static Object expected;
    public static int expectedMark;
    public static long expectedStamp;
    public static void event(int value) {
        events = events * 10 + value;
        if (value >= 2 && (current.base != expected
                || current.mark != (value >= 3 ? expectedMark : 0)
                || current.stamp != (value >= 4 ? expectedStamp : 0)))
            throw new AssertionError("delegation postlude observation");
        if (failAt == value) throw failure;
    }
}
