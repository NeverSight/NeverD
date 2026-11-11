package fixture;
public final class Trace {
    public static int trace;
    public static int failAt;
    public static RuntimeException failure;
    public static void step(int value) {
        trace = trace * 10 + value;
        if (failAt == value) throw failure;
    }
}
