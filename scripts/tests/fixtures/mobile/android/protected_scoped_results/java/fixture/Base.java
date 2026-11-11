package fixture;
public abstract class Base<T, V> {
    protected abstract T make();
    protected abstract int size(T input);
    protected abstract T copy(T input);
    protected abstract T narrow(Box<? extends V> input);
    public T callMake() { return make(); }
    public int callSize(T input) { return size(input); }
    public T callCopy(T input) { return copy(input); }
    public T callNarrow(Box<? extends V> input) { return narrow(input); }
}
