package fixture;
public final class Trace {
    public static Factory hook;
    public static Core core;
    private Trace() {}
    public static void hit(int phase, Object value) { hook.stage(phase, value); }
}
