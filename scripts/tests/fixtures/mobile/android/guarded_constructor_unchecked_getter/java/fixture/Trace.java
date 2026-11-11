package fixture;

public final class Trace {
    private Trace() {}
    public static Object value, type, baseValue, marker;
    public static Object[] arguments;
    public static String name;
    public static Runnable action;
    public static int calls, bases, objects, subjects, directs, stage, arity;
}
