package fixture;

public final class Trace {
    private Trace() {}
    public static Object value, store, saved, element, marker;
    public static Object[] values;
    public static Runnable action;
    public static boolean flag, afterFlag, inspect, write;
    public static long stamp, afterStamp;
    public static int calls, forwards, callbacks, stage, entry, index;
}
