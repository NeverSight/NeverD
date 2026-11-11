package fixture;
public final class Trace {
    private Trace() {}
    public static Iterable input;
    public static Matcher matcher;
    public static Object result,marker;
    public static Runnable action;
    public static int calls,stage;
}
