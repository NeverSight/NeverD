package fixture;

public abstract class Trace {
    public static Trace hook;
    public static Object value;
    public static boolean replace;
    public static Object[] replacement;
    public Trace() {}
    public abstract void observe(int phase, Base subject, Object[] input);
    public static void hit(int phase, Base subject, Object[] input) { hook.observe(phase, subject, input); }
}
