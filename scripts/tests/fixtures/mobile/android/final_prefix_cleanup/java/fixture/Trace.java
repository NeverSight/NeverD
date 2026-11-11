package fixture;
public abstract class Trace {
    public static Core leaked;
    public static Trace hook;
    public static Object first, second;
    public static boolean forceReject;
    public static RuntimeException rejection;
    public Trace() {}
    public abstract void observe(int phase, Core owner, Object value) throws Throwable;
    public static void hit(int phase, Core owner, Object value) throws Throwable { hook.observe(phase, owner, value); }
}
