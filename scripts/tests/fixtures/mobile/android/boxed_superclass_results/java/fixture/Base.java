package fixture;
public abstract class Base<T> {
    protected Base() {}
    public abstract T get(int index);
    public T parentGet(int index) { return get(index); }
}
