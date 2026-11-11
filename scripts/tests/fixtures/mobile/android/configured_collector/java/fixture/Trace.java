package fixture;

public final class Trace {
    private Trace() {}
    public static int targets, shortCalls;
    public static long events;
    public static Object source, seen, mapper, result;
    public static CharSequence separator, prefix, suffix, prefixConfig, suffixConfig, mapped;
    public static int separatorLength, prefixLength, suffixLength;
    public static Runnable action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
