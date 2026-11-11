package fixture;
public abstract class Base<A, R> {
    public int visits;
    protected abstract R convert(A input);
    public R dispatch(A input) { visits++; return convert(input); }
}
