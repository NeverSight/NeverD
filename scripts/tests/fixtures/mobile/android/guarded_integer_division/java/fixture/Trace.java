package fixture;
public final class Trace {
    private Trace() {}
    public static Object owner,marker;
    public static Runnable action;
    public static RuntimeException failure;
    public static int calls,checks,stage;
    public static boolean condition;
    public static void require(boolean condition) {
        Trace.checks++;
        Trace.condition=condition;
        if(!condition)throw failure;
    }
}
