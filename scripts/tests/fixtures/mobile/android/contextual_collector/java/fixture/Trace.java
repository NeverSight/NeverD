package fixture;

public final class Trace {
    private Trace() {}
    public static int targets, shortCalls;
    public static long events;
    public static Object result;
    public static CharSequence separator, prefix, suffix, prefixConfig, suffixConfig;
    public static int separatorLength, prefixLength, suffixLength;
    public static Runnable before, action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
