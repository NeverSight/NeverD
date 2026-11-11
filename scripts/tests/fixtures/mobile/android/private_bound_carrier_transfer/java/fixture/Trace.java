package fixture;

public abstract class Trace {
    public static Trace hook;
    public static Object value;
    public static boolean replace;
    public static Slot<?> replacement;
    public Trace() {}
    public abstract void observe(int phase, Core subject, Slot<?> input);
    public static void hit(int phase, Core subject, Slot<?> input) { hook.observe(phase, subject, input); }
}
