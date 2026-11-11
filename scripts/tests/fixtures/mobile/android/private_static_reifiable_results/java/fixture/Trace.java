package fixture;

public final class Trace {
    public static Object value;
    public static Class<?> token;
    public static Class<?>[] array;
    public static Action action;
    public static String name;
    public static long stamp;
    public static int calls, callbacks, stage;
    private Trace() {}
}
