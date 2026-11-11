package fixture;
public abstract class Base<S> {
    public int visits;
    protected abstract S compute(Class<?> type);
    public S get(Class<?> type) { visits++; return compute(type); }
}
