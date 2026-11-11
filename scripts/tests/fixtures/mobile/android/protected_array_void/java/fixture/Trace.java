package fixture;

public abstract class Trace {
    public static Trace hook;
    public static Object value;
    public abstract void observe(int phase, Base owner, Object[] input, int index, Object value);
    public static void hit(int phase, Base owner, Object[] input, int index, Object value) {
        hook.observe(phase, owner, input, index, value);
    }
}
