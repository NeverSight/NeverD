package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Owner owner;
    public static Target last;
    public Trace() {}
    public abstract void observe(int phase, Target target);
    public static void hit(int phase, Target target) { hook.observe(phase, target); }
}
