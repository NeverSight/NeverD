package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Object self, Object value) {}
    public static void hit(int phase, Object self) {
        if (hook != null) hook.observe(phase, self, null);
    }
    public static Base value(Object self, Base value) {
        if (hook != null) hook.observe(2, self, value);
        return value;
    }
    public static Object token(Object self, Object value) {
        if (hook != null) hook.observe(3, self, value);
        return value;
    }
}
