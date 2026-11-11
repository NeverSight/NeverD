package fixture;

public final class Trace {
    public static int calls;
    public static int events;
    public static OrderNode receiver;
    public static OrderNode argument;
    public static RuntimeException failure;

    private Trace() {}
}
