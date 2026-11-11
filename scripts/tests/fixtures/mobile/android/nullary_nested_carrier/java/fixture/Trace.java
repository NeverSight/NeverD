package fixture;

public final class Trace {
    private Trace() {}

    public static Object value;
    public static Runnable action;
    public static RuntimeException initFailure;
    public static int initializations;
    public static int calls;
    public static int callbacks;
    public static int stage;
}
