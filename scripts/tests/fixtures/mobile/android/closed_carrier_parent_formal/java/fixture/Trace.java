package fixture;

public final class Trace {
    private Trace() {}
    public static Base<?> active;
    public static Object source, token;
    public static java.util.Collection<?> extras;
    public static Runnable action;
    public static int targets, children, stage, before;
}
