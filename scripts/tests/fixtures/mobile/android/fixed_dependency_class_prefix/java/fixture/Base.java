package fixture;
public abstract class Base<T> extends Root {
    public abstract T item();
    public abstract void set(T value);
}
