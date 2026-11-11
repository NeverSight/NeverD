package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Result result, alternative;
    public Trace() {}
    public abstract void observe(int phase, Object value, Property<?, ?> property, Evaluator<?> evaluator, Object[] values);
    public static void hit(int phase, Object value, Property<?, ?> property, Evaluator<?> evaluator, Object[] values) {
        hook.observe(phase, value, property, evaluator, values);
    }
}
