package fixture;
public final class Child<C> extends Middle<C> {
    public int selects;
    public C seenFallback;
    public Error selectFailure;
    public Child() {}
    @Override
    protected C select(C fallback) {
        selects++;
        events = events * 10 + 2;
        seenFallback = fallback;
        if (selectFailure != null) throw selectFailure;
        return fallback == null ? result : fallback;
    }
}
