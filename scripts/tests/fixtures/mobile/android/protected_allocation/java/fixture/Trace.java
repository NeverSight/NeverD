package fixture;
public final class Trace {
    public static int value;
    public static int failAt;
    public static boolean failFallback;
    public static Object token;
    public static RuntimeException failure;
    public static RuntimeException fallbackFailure;
    public static void step(int code) {
        value = value * 10 + code;
        if (code == 4 && failFallback) throw fallbackFailure;
        if (code == failAt) throw failure;
    }
    public static void prepare() { step(2); }
}
