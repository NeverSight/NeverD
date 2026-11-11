package fixture;

public class Trace {
    public static Trace hook;
    public static boolean returned;
    public Trace() {}
    public void observe(int phase, Object carrier, Object values, Object value) {}
    public static void hit(int phase, Object carrier, Object values, Object value) {
        Trace current = hook;
        if (current != null) current.observe(phase, carrier, values, value);
    }
}
