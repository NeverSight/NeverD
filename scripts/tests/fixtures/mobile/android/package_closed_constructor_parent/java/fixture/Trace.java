package fixture;

public abstract class Trace {
    public static Trace hook;
    public abstract void observe(int phase, Parent owner, Object value, int number);
    public static void hit(int phase, Parent owner, Object value, int number) {
        hook.observe(phase, owner, value, number);
    }
}
