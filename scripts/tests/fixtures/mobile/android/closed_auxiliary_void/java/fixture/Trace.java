package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Object receiver, Object token, Object callback, Object result) {}
    public static void hit(int phase, Object receiver, Object token, Object callback, Object result) {
        hook.observe(phase, receiver, token, callback, result);
    }
}
