package fixture;
public abstract class Trace {
    public static Trace hook;
    public Trace() {}
    public abstract void observe(int phase, Api object);
    public static void hit(int phase, Api object) { hook.observe(phase, object); }
}
