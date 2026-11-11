package fixture;

public final class Trace {
    private Trace() {}
    public static Object[] input;
    public static Gate<?> gate;
    public static Object first, value;
    public static Runnable action;
    public static boolean accepted;
    public static int calls, relays, stage;
}
