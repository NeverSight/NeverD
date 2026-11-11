package fixture;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Object array, int length, Object token, Object result) {}
    public static void hit(int phase, Object array, int length, Object token, Object result) {
        Trace current = hook;
        if (current != null) current.observe(phase, array, length, token, result);
    }
}
