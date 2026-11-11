package fixture;
public final class Trace {
    public static int trace, failAt;
    public static Object seen, result;
    public static Token token;
    public static RuntimeException failure;
    public static void step(int value) {
        trace = trace * 10 + value;
        if (value == failAt) throw failure;
    }
}
