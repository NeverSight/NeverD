package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Result result, alternative;
    public Trace() {}
    public abstract void observe(int phase, Object value, Property<?, ?> property, Converter<?, ?> converter, Context context);
    public static void hit(int phase, Object value, Property<?, ?> property, Converter<?, ?> converter, Context context) {
        hook.observe(phase, value, property, converter, context);
    }
}
