package fixture;
public final class Trace {
    public static long trace;
    public static int failAt, flag;
    public static Object first, second, callbackResult;
    public static Carrier output;
    public static RuntimeException failure;
    public static Error initFatal;
    public static void step(int value) {
        trace = trace * 10 + value;
        if (value == failAt) throw failure;
    }
}
