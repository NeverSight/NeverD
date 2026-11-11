package fixture;
public abstract class Base<T> {
    public int visits;
    protected abstract T get(Token token, int index, long wide, double fraction);
    public T dispatch(Token token, int index, long wide, double fraction) { visits++; return get(token, index, wide, fraction); }
}
