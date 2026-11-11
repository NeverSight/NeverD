package fixture;
public abstract class Trace {
    public static Trace hook;
    public static boolean forceContainsFalse;
    public static int emptyOverride;
    public static Carrier alternative;
    public Trace() {}
    public abstract void observe(int phase, Carrier owner, Object seed);
    public static void hit(int phase, Carrier owner, Object seed) { hook.observe(phase, owner, seed); }
}
