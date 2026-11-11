package fixture;

import java.util.Collection;
import java.util.Map;

public final class Trace {
    private Trace() {}
    public static int targets;
    public static long events;
    public static Object source, seen, left, right, result;
    public static Object first, second, collectionSupplier, mapSupplier;
    public static Collection collection;
    public static Map map;
    public static Runnable action;
    public static void hit(int digit) { events = events * 10 + digit; }
}
