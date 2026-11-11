package fixture;
public abstract class Base<T, E> {
    public int visits;
    protected abstract Box<E> read(T input);
    public Box<E> dispatch(T input) { visits++; return read(input); }
}
