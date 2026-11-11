package fixture;
public abstract class Parent<P> implements Api<P> {
    public P result;
    public Object seenInput;
    public int calls, events;
    public RuntimeException failure;
    public Parent() {}
    @Override
    public P convert(Object input, P fallback) {
        calls++;
        events = events * 10 + 1;
        seenInput = input;
        if (failure != null) throw failure;
        return select(fallback);
    }
    protected P select(P fallback) {
        events = events * 10 + 3;
        return fallback == null ? result : fallback;
    }
}
