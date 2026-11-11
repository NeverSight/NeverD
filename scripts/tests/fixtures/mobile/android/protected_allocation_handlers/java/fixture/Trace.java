package fixture;
public final class Trace {
    public static Trace observer, replacement;
    public static Throwable logged;
    public static boolean failRecord;
    public Throwable seen;
    public Object context;
    public int code;
    public boolean checked;
    public static int value;
    public static int failAt;
    public static boolean failFallback;
    public static Object token;
    public static RuntimeException failure;
    public static RuntimeException fallbackFailure;
    public static void step(int code) {
        value = value * 10 + code;
        if (code == 5 && failRecord) throw fallbackFailure;
        if (code == 4 && failFallback) throw fallbackFailure;
        if (code == failAt) throw failure;
    }
    public static void prepare() { observer = replacement; step(2); }
    public static int logFailure(String first, String second, Throwable problem) {
        logged = problem;
        step(5);
        return first.length() + second.length();
    }
}
