package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Object task, Object value, int attempts) {}
    public static void hit(int phase, Object task, Object value, int attempts) {
        if (hook != null) hook.observe(phase, task, value, attempts);
    }
}
