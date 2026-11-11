package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Subject receiver, Callback<?> callback, Object value) {}
    public static void hit(int phase, Subject receiver, Callback<?> callback, Object value) {
        hook.observe(phase, receiver, callback, value);
    }
}
