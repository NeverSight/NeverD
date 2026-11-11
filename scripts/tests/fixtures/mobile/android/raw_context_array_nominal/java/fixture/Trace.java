package fixture;
public abstract class Trace {
    public static Trace hook;
    public static Result result, alternative;
    public Trace() {}
    public abstract void observe(int phase, Property<?, ?> property, Evaluator<?> evaluator, Object[] values);
    public static void hit(int phase, Property<?, ?> property, Evaluator<?> evaluator, Object[] values) {
        hook.observe(phase, property, evaluator, values);
    }
}
