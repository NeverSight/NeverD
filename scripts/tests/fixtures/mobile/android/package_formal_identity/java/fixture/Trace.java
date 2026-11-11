package fixture;
public class Trace {
    public static Trace hook;
    public static Object next;
    public Trace() {}
    public void observe(int phase, Object value) {}
    public static void hit(int phase, Object value) {
        if (hook != null) hook.observe(phase, value);
    }
}
