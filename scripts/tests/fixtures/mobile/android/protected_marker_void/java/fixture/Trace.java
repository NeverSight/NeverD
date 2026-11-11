package fixture;

public abstract class Trace {
    public static Trace hook;
    public abstract void observe(int phase, Base owner, Marker input, Base context);
    public static void hit(int phase, Base owner, Marker input, Base context) {
        hook.observe(phase, owner, input, context);
    }
}
