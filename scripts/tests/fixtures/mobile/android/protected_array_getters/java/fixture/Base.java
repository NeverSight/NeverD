package fixture;
public abstract class Base<T> {
    public int visits;
    protected abstract T get();
    public T dispatch() { visits++; return get(); }
}
