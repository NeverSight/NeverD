package fixture;

public final class Trace {
    public static int calls;
    public static int events;
    public static Ordered receiver;
    public static Ordered argument;
    public static RuntimeException failure;

    private Trace() {}
}
