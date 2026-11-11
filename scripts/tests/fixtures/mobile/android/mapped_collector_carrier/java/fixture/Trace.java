package fixture;
public final class Trace {
    private Trace() {}
    public static Object mapper, downstream, value, marker;
    public static Runnable action;
    public static int calls, reverses, stage;
}
