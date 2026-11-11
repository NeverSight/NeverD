package fixture;
public final class Trace {
    private Trace() {}
    public static int targets;
    public static long events;
    public static Object seed, mapper, operator, source, input, mapped, folded, result;
    public static Runnable action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
