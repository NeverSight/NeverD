package fixture;
public abstract class Base<A, V> {
    public int visits;
    protected abstract Result<V> convert(A input);
    public Result<V> dispatch(A input) { visits++; return convert(input); }
}
