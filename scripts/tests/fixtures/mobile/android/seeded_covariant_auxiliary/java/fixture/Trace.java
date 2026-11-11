package fixture;

public final class Trace {
    private Trace() {}

    public static Object seed;
    public static Object value;
    public static Class<? extends Runnable> type;
    public static Runnable action;
    public static Provider receiver;
    public static String name;
    public static int calls;
    public static int instances;
    public static int overrides;
    public static int callbacks;
    public static int stage;
}
