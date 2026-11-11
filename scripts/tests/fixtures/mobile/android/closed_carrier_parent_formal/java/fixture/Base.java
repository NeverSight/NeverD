package fixture;

public abstract class Base<T> {
    public final T value;
    public final java.util.Collection<Payload> extras;
    public final Object token;
    protected Base(T value, Runnable action, java.util.Collection<Payload> extras, Object token) {
        Trace.targets++;
        Trace.stage = 1;
        Trace.active = this;
        Trace.source = value;
        Trace.extras = extras;
        Trace.token = token;
        Trace.before = state();
        Trace.stage = 2;
        action.run();
        Trace.stage = 3;
        this.value = value;
        this.extras = extras;
        this.token = token;
    }
    protected abstract int state();
    public T get() { return value; }
}
