package fixture;
public final class Trace {
    private Trace() {}
    public static Iterable input;
    public static Object result,marker,built,inner,key;
    public static Runnable action;
    public static int calls,stage;
}
