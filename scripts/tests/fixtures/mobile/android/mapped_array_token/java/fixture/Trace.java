package fixture;

public final class Trace {
    public static Object source, token, mapper, mapped, value, marker;
    public static String name;
    public static Runnable action;
    public static int calls, relays, stage;
    private Trace() {}
}
