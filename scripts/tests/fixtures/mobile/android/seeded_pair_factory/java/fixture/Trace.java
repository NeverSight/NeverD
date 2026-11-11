package fixture;
public class Trace {
    public static Trace hook;
    public static Object next;
    public Trace() {}
    public void observe(int phase, Object left, Object right, Object result) {}
    public static void hit(int phase, Object left, Object right, Object result) {
        if (hook != null) hook.observe(phase, left, right, result);
    }
}
