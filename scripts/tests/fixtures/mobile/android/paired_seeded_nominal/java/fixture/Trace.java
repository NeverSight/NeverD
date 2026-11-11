package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Result result, alternative;
    public Trace() {}
    public abstract void observe(int phase, Object value, Property<?, ?> property, Context context, Token token);
    public static void hit(int phase, Object value, Property<?, ?> property, Context context, Token token) {
        hook.observe(phase, value, property, context, token);
    }
}
