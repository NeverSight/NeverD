package fixture;

public final class Trace {
    public static int initializations, allocations, callbacks, parents, targets, stage;
    public static Object created, partial, key, value;
    public static long tag;
    public static Runnable action;
    public static RuntimeException initFailure;
    private Trace() {}
}
