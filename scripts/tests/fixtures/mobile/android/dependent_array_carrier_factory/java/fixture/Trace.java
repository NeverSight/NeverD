package fixture;
public class Trace {
    public static Trace hook;
    public static Object output;
    public Trace() {}
    public void observe(int phase, Object token, Object input, Object value, Object result) {}
    public static void hit(int phase, Object token, Object input, Object value, Object result) {
        hook.observe(phase, token, input, value, result);
    }
}
