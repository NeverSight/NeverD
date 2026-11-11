package fixture;

public final class Trace {
    private Trace() {}
    public static Object seed, first, value;
    public static String label;
    public static Object[] values;
    public static Runnable action;
    public static int calls, relays, stage;
}
