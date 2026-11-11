package fixture;
public final class Trace {
    public static Factory hook;
    public static Core core;
    public static void hit(int phase, Object value) {
        if (hook != null) hook.stage(phase, value);
    }
}
