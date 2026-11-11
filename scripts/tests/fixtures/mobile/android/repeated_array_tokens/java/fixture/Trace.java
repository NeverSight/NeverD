package fixture;
public final class Trace {
    private Trace() {}
    public static Object source, token, marker;
    public static volatile Object value;
    public static String name;
    public static Runnable action;
    public static int calls, relays, stage, size;
}
