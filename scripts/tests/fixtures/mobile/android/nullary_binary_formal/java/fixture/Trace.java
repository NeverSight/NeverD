package fixture;
public final class Trace {
    private Trace() {}
    public static Object result,marker,seen,applied;
    public static Runnable action;
    public static int calls,stage,applies;
}
