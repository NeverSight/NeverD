package fixture;
public abstract class Base<T> {
    public int calls, callbacks;
    public Object seen;
    public int[] marks;
    public RuntimeException failure;
    protected abstract int size(T value);
    public int call(T value) { callbacks++; return size(value); }
}
