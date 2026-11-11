package fixture;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Context context, String key, Class token, Object result) {}
    public static void hit(int phase, Context context, String key, Class token, Object result) {
        Trace current = hook;
        if (current != null) current.observe(phase, context, key, token, result);
    }
}
