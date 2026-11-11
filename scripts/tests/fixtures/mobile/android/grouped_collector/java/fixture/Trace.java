package fixture;

import java.util.Map;

public final class Trace {
    private Trace() {}
    public static int targets;
    public static long events;
    public static Object source, seen, key, state, value, result;
    public static Object classifier, mapSupplier, downstream;
    public static Map map;
    public static Runnable action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
