package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Subject receiver, Object value) {}
    public static void hit(int phase, Subject receiver, Object value) {
        hook.observe(phase, receiver, value);
    }
}
