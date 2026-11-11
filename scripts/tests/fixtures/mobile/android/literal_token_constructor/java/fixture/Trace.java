package fixture;
public final class Trace {
    public static int trace;
    public static int failAt;
    public static Object seen;
    public static RuntimeException failure;
    public static void step(int value) {
        trace = trace * 10 + value;
        if (failAt == value) throw failure;
    }
    public static int prepare(Object value) {
        step(2);
        seen = value;
        return value == null ? -13 : 29;
    }
}
