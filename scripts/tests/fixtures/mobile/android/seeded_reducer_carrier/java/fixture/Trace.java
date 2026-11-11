package fixture;
public final class Trace {
    private Trace() {}
    public static Object seed, operator, value, marker;
    public static Runnable action;
    public static int calls, reverses, stage;
}
