package fixture;
public class Trace {
    public static Trace hook;
    public static Object core;
    public Trace() {}
    public void stage(int phase, Object value) {}
    public static void hit(int phase, Object value) { hook.stage(phase, value); }
}
