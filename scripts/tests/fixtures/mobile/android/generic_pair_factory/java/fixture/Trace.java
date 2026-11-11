package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Object first, Object second, Object third) {}
    public static void hit(int phase, Object first, Object second, Object third) {
        if (hook != null) hook.observe(phase, first, second, third);
    }
}
