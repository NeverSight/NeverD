package fixture;

public final class Trace {
    private Trace() {}
    public static Object value;
    public static String context, label;
    public static Runnable action;
    public static Class<?>[] tokens;
    public static Class<?> first;
    public static long argument, stamp;
    public static int calls, callbacks, relays, stage, length;
}
