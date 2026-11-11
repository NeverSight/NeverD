package fixture;

public final class Trace {
    private Trace() {}
    public static Object receiver, first, value;
    public static java.lang.reflect.Method method;
    public static Object[] values;
    public static Runnable action;
    public static int calls, relays, stage;
}
